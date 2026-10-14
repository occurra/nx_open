// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "ffmpeg_video_decoder.h"

extern "C" {
#include <libavutil/cpu.h>
#include <libavutil/imgutils.h>
} // extern "C"

#include <nx/codec/h264/common.h>
#include <nx/codec/h264/sequence_parameter_set.h>
#include <nx/codec/nal_units.h>
#include <nx/media/codec_parameters.h>
#include <nx/media/ffmpeg/av_packet.h>
#include <nx/media/ffmpeg/ffmpeg_utils.h>
#include <nx/media/ffmpeg/shared_memory_frame_allocator.h>
#include <nx/media/utils.h>
#include <nx/metric/metrics_storage.h>
#include <nx/utils/log/log.h>
#include <nx/utils/math/math.h>
#include <utils/media/frame_type_extractor.h>

static const int LIGHT_CPU_MODE_FRAME_PERIOD = 2;

namespace {

bool isImage(const QnConstCompressedVideoDataPtr& data)
{
    return data->compressionType == AV_CODEC_ID_PNG
        || data->flags & QnAbstractMediaData::MediaFlags_StillImage;
}

} // namespace

QnFfmpegVideoDecoder::QnFfmpegVideoDecoder(
    const DecoderConfig& config,
    nx::metric::Storage* metrics,
    const QnConstCompressedVideoDataPtr& data):
    m_context(0),
    m_decodeMode(DecodeMode_Full),
    m_newDecodeMode(DecodeMode_NotDefined),
    m_lightModeFrameCounter(0),
    m_currentWidth(-1),
    m_currentHeight(-1),
    m_forceSliceDecoding(-1),
    m_prevSampleAspectRatio(1.0),
    m_prevTimestamp(AV_NOPTS_VALUE),
    m_mtDecodingPolicy(config.mtDecodePolicy),
    m_useMtDecoding(config.mtDecodePolicy == MultiThreadDecodePolicy::enabled),
    m_needRecreate(false),
    m_metrics(metrics),
    m_config(config)
{
    if (m_metrics)
        m_metrics->decoders()++;
    m_opened = openDecoder(data);
}

QnFfmpegVideoDecoder::~QnFfmpegVideoDecoder(void)
{
    avcodec_free_context(&m_context);
    if (m_metrics)
        m_metrics->decoders()--;
}

void QnFfmpegVideoDecoder::determineOptimalThreadType(const QnConstCompressedVideoDataPtr& data)
{
    // Don't use multithreaded decoding for images, since then ffmpeg increases
    // AVCodecContext->delay to more than 0 and does not output frames on single call
    // decode(without flushing).
    if (m_useMtDecoding && !isImage(data))
        m_context->thread_count = std::min(kMaxDecodeThread, av_cpu_count());
    else
        m_context->thread_count = 1; //< Turn off multi thread decoding.

    NX_DEBUG(
        this,
        "Initialize video decoder, codec id: %1, thread count: %2",
        m_context->codec_id,
        m_context->thread_count);

    if (m_forceSliceDecoding == -1 && data && data->data()
        && m_context->codec_id == AV_CODEC_ID_H264)
    {
        m_forceSliceDecoding = 0;
        int nextSliceCnt = 0;
        const quint8* curNal = (const quint8*) data->data();
        if (curNal[0] == 0)
        {
            const quint8* end = curNal + data->dataSize();
            for (curNal = nx::media::nal::findNextNAL(curNal, end); curNal < end;
                 curNal = nx::media::nal::findNextNAL(curNal, end))
            {
                const auto nalType = nx::media::h264::decodeType(*curNal);
                if (nalType >= nx::media::h264::nuSliceNonIDR
                    && nalType <= nx::media::h264::nuSliceIDR)
                {
                    nx::utils::BitStreamReader bitReader;
                    try
                    {
                        bitReader.setBuffer(curNal + 1, end);
                        int first_mb_in_slice = bitReader.getGolomb();
                        if (first_mb_in_slice > 0)
                        {
                            nextSliceCnt++;
                            // break;
                        }
                    }
                    catch (...)
                    {
                        break;
                    }
                }
            }
            if (nextSliceCnt >= 3)
                m_forceSliceDecoding =
                    1; // multislice frame. Use multislice decoding if slice count 4 or above
        }
    }

    m_context->thread_type = m_context->thread_count > 1 && (m_forceSliceDecoding != 1)
        ? FF_THREAD_FRAME
        : FF_THREAD_SLICE;

    if (m_context->codec_id == AV_CODEC_ID_H264 && m_context->thread_type == FF_THREAD_SLICE)
    {
        // ignoring deblocking filter type 1 for better performance between H264 slices.
        m_context->flags2 |= AV_CODEC_FLAG2_FAST;
    }

    if (m_config.forceGrayscaleDecoding)
        m_context->flags |= AV_CODEC_FLAG_GRAY;
}

bool QnFfmpegVideoDecoder::openDecoder(const QnConstCompressedVideoDataPtr& data)
{
    auto codec = avcodec_find_decoder(data->compressionType);
    if (codec == nullptr)
    {
        NX_ERROR(
            this,
            "Failed to open ffmpeg video decoder, codec not found: %1",
            data->compressionType);
        return false;
    }
    m_context = avcodec_alloc_context3(codec);
    if (data->context)
    {
        data->context->toAvCodecContext(m_context);
        // TODO make correct check on changing decoder settings
        if (m_context->width > 8 && m_context->height > 8 && m_currentWidth == -1)
        {
            m_currentWidth = m_context->width;
            m_currentHeight = m_context->height;
        }
    }

    m_frameTypeExtractor = std::make_unique<FrameTypeExtractor>(
        CodecParametersConstPtr(new CodecParameters(m_context)));

    determineOptimalThreadType(data);

    int status = avcodec_open2(m_context, codec, NULL);
    if (status < 0)
    {
        NX_ERROR(
            this,
            "Failed to open ffmpeg video decoder, error: %1",
            nx::media::ffmpeg::avErrorToString(status));
        return false;
    }
    m_codecId = data->compressionType;
    return true;
}

void QnFfmpegVideoDecoder::flushBuffers()
{
    if (m_context)
        avcodec_flush_buffers(m_context);
    // Frames already drained from the decoder belong to the pre-flush position.
    m_outputFrames.clear();
}

bool QnFfmpegVideoDecoder::resetDecoder(const QnConstCompressedVideoDataPtr& data)
{
    NX_DEBUG(this, "Reset video decoder: %1", data);
    if (!data || !(data->flags & AV_PKT_FLAG_KEY))
    {
        m_needRecreate = true;
        return true; // can't reset right now
    }

    avcodec_free_context(&m_context);
    // Any backlog belongs to the decoder instance being replaced; carrying it over could hand
    // out frames with a stale resolution/codec after this reset.
    m_outputFrames.clear();
    m_opened = openDecoder(data);
    m_needRecreate = false;
    return m_opened;
}

void QnFfmpegVideoDecoder::processNewResolutionIfChanged(
    const QnConstCompressedVideoDataPtr& data,
    int width,
    int height)
{
    if (m_currentWidth == -1)
    {
        m_currentWidth = width;
        m_currentHeight = height;
    }
    else if (width != m_currentWidth || height != m_currentHeight)
    {
        m_currentWidth = width;
        m_currentHeight = height;
        resetDecoder(data);
    }
    if (data->flags.testFlag(QnAbstractMediaData::MediaFlags_newCodecParams))
        resetDecoder(data);
}

void QnFfmpegVideoDecoder::setMultiThreadDecoding(bool value)
{
    if (value != m_useMtDecoding)
    {
        NX_DEBUG(this, "Set multithreaded decoding: %1", value);
        m_useMtDecoding = value;
        m_needRecreate = true;
    }
}

void QnFfmpegVideoDecoder::setGreyOnlyMode(bool value)
{
    if (!m_context)
        return;

    if (value)
        m_context->flags |= AV_CODEC_FLAG_GRAY;
    else
        m_context->flags &= ~AV_CODEC_FLAG_GRAY;
}

bool QnFfmpegVideoDecoder::decodeVideo(const QnConstCompressedVideoDataPtr& data)
{
    nx::media::ffmpeg::AvPacket avPacket(data.get());
    auto packet = avPacket.get();
    if (data && isImage(data))
        packet->flags = AV_PKT_FLAG_KEY;

    // Frames are retrieved in the drain loop below, right after this send.
    m_lastDecodeResult = avcodec_send_packet(m_context, packet);
    if (m_lastDecodeResult == AVERROR_EOF)
        m_lastDecodeResult = 0;

    if (m_lastDecodeResult < 0)
    {
        NX_DEBUG(
            this,
            "Ffmpeg decoder error: %1",
            nx::media::ffmpeg::avErrorToString(m_lastDecodeResult));
        return false;
    }
    // A single avcodec_send_packet() can make more than one frame available (reordering
    // catching up, flushing, etc). Pull all of them out now and queue them, so the decoder's
    // output queue is empty by the next call -- keeping avcodec_send_packet() from ever
    // refusing the next packet with EAGAIN -- and so no ready frame is left behind.
    while (true)
    {
        CLVideoDecoderOutputPtr frame(new CLVideoDecoderOutput());
        m_lastDecodeResult = avcodec_receive_frame(m_context, frame.get());
        if (m_lastDecodeResult == AVERROR(EAGAIN) || m_lastDecodeResult == AVERROR_EOF)
        {
            // No more frames are ready right now -- done draining successfully.
            m_lastDecodeResult = 0;
            break;
        }

        if (m_lastDecodeResult < 0)
        {
            NX_DEBUG(
                this,
                "Ffmpeg decoder error: %1",
                nx::media::ffmpeg::avErrorToString(m_lastDecodeResult));
            return false;
        }
        if (m_metrics)
            m_metrics->decodedPixels() += frame->width * frame->height;

        if (!finalizeDecodedFrame(data, frame.get()))
            return false;

        m_outputFrames.push_back(frame);
    }
    return true;
}

bool QnFfmpegVideoDecoder::finalizeDecodedFrame(
    const QnConstCompressedVideoDataPtr& data,
    CLVideoDecoderOutput* outFrame)
{
    if (m_config.forceRgbaFormat && outFrame->format != AV_PIX_FMT_RGBA)
    {
        CLVideoDecoderOutput tmpFrame;
        tmpFrame.reallocate(outFrame->width, outFrame->height, AV_PIX_FMT_RGBA);
        if (data)
            tmpFrame.pkt_dts = data->timestamp;

        if (outFrame->convertTo(&tmpFrame))
        {
            outFrame->copyFrom(&tmpFrame);
            outFrame->sample_aspect_ratio = getSampleAspectRatio();
            outFrame->flags = m_lastFlags;
            outFrame->channel = m_lastChannelNumber;
            // See the rationale on the pts fallback in the non-RGBA branch below.
            if (outFrame->pts == AV_NOPTS_VALUE)
                outFrame->pts = outFrame->pkt_dts;
            return m_context->pix_fmt != AV_PIX_FMT_NONE;
        }
    }

    outFrame->format = CLVideoDecoderOutput::fixDeprecatedPixelFormat(m_context->pix_fmt);
    outFrame->sample_aspect_ratio = getSampleAspectRatio();
    outFrame->flags = m_lastFlags;
    outFrame->channel = m_lastChannelNumber;
    // In case of b-frames stream ffmpeg gives last flushed frames with AV_NOPTS_VALUE, so use
    // m_prevFrameDuration to get frame pts.
    if (!data && outFrame->pkt_dts == AV_NOPTS_VALUE)
    {
        outFrame->pkt_dts = m_prevTimestamp + m_prevFrameDuration;
        m_prevTimestamp = outFrame->pkt_dts;
    }
    // Source packets carry the presentation timestamp in data->timestamp,
    // which we feed to the decoder via packet.dts (because RTSP-style
    // streams supply only one timestamp). FFmpeg propagates that to
    // pkt_dts on the output frame, and for B-frame streams it also
    // computes pts via POC reordering — but for intra-only / no-reorder
    // streams it leaves pts unset. Make pts authoritative for callers
    // by falling back to pkt_dts when FFmpeg didn't fill it.
    if (outFrame->pts == AV_NOPTS_VALUE)
        outFrame->pts = outFrame->pkt_dts;
    return m_context->pix_fmt != AV_PIX_FMT_NONE;
}

// The input buffer must be AV_INPUT_BUFFER_PADDING_SIZE larger than the actual bytes read because
// some optimized bitstream readers read 32 or 64 bits at once and could read over the end. The end
// of the input buffer buf should be set to 0 to ensure that no overreading happens for damaged
// MPEG streams.
bool QnFfmpegVideoDecoder::decode(
    const QnConstCompressedVideoDataPtr& data,
    CLVideoDecoderOutput* const outFrame)
{
    if (data && m_codecId != data->compressionType)
    {
        if (!data->flags.testFlag(QnAbstractMediaData::MediaFlags_AVKey))
        {
            NX_DEBUG(
                this,
                "Decoding should start from key frame, skip. codec: %1, timestamp: %2",
                data->compressionType,
                data->timestamp);
            return false; //< Can't switch decoder to new codec right now.
        }
        if (!resetDecoder(data))
            return false;
    }
    if (!m_opened)
        return false;

    outFrame->clean();

    if (data)
    {
        m_lastFlags = data->flags;
        m_lastChannelNumber = data->channelNumber;

        if (m_context == 0)
        {
            NX_WARNING(this, "Decoder not found, codec: %1", data->compressionType);
            return false;
        }

        if (m_newDecodeMode != DecodeMode_NotDefined && (data->flags & AV_PKT_FLAG_KEY))
        {
            m_decodeMode = m_newDecodeMode;
            m_newDecodeMode = DecodeMode_NotDefined;
            m_lightModeFrameCounter = 0;
        }

        if ((m_decodeMode > DecodeMode_Full
             || (data->flags & QnAbstractMediaData::MediaFlags_PreRoll))
            && data->data())
        {
            if (data->compressionType == AV_CODEC_ID_MJPEG)
            {
                uint period = m_decodeMode == DecodeMode_Fast ? LIGHT_CPU_MODE_FRAME_PERIOD
                                                              : LIGHT_CPU_MODE_FRAME_PERIOD * 2;
                if (m_lightModeFrameCounter < period)
                {
                    ++m_lightModeFrameCounter;
                    return false;
                }
                else
                    m_lightModeFrameCounter = 0;
            }
            else if (!(data->flags & AV_PKT_FLAG_KEY))
            {
                if (m_decodeMode == DecodeMode_Fastest)
                    return false;
                else if (
                    m_frameTypeExtractor->getFrameType(
                        (quint8*) data->data(),
                        static_cast<int>(data->dataSize()))
                    == FrameTypeExtractor::B_Frame)
                    return false;
            }
        }

        if (m_needRecreate && (data->flags & AV_PKT_FLAG_KEY))
        {
            if (!resetDecoder(data))
                return false;
        }

        // ### handle errors
        if (m_context->pix_fmt == -1)
            m_context->pix_fmt = AVPixelFormat(0);

        if (data->context)
        {
            if (data->context->getWidth() != 0 && data->context->getHeight() != 0)
                processNewResolutionIfChanged(
                    data,
                    data->context->getWidth(),
                    data->context->getHeight());
        }

        if (m_context->codec)
        {
            if (!decodeVideo(data))
                return false;

            if (m_outputFrames.empty() && data->flags & QnAbstractMediaData::MediaFlags_StillImage)
            {
                if (!decodeVideo(nullptr))
                    return false;
            }
        }

        if (m_mtDecodingPolicy == MultiThreadDecodePolicy::autoDetect)
        {
            if (m_context->width <= 3500)
                setMultiThreadDecoding(false);
            else
            {
                int64_t frameDistance = data->timestamp - m_prevTimestamp;
                if (frameDistance > 0)
                {
                    const double fps = 1'000'000.0 / frameDistance;
                    int64_t megapixels = m_context->width * m_context->height * fps;
                    static const int64_t kThreshold = 1920 * 2 * 1080 * 2 * 20; //< 4k, 20fps
                    if (megapixels >= kThreshold)
                        setMultiThreadDecoding(true); // high fps and high resolution
                }
            }
        }
        if (m_prevTimestamp != AV_NOPTS_VALUE)
            m_prevFrameDuration = data->timestamp - m_prevTimestamp;

        m_prevTimestamp = data->timestamp;
    }
    else
    {
        decodeVideo(nullptr); // flush
    }

    // Hand out the oldest ready frame first, so a burst of reordered output is only delayed
    // across calls, never dropped. The caller owns the output frame, so move the queued frame's
    // buffers into it instead of handing out the queued object itself.
    if (m_outputFrames.empty())
        return false;

    const CLVideoDecoderOutputPtr frame = m_outputFrames.front();
    m_outputFrames.pop_front();
    outFrame->assignMiscData(frame.get());
    av_frame_move_ref(outFrame, frame.get());
    return true;
}

double QnFfmpegVideoDecoder::getSampleAspectRatio() const
{
    if (!m_context || m_context->width <= 8 || m_context->height <= 8)
        return m_prevSampleAspectRatio;

    double result = av_q2d(m_context->sample_aspect_ratio);
    if (qAbs(result) >= 1e-7)
    {
        m_prevSampleAspectRatio = result;
        return result;
    }

    QSize srcSize(m_context->width, m_context->height);
    m_prevSampleAspectRatio = nx::media::getDefaultSampleAspectRatio(srcSize);
    return m_prevSampleAspectRatio;
}

bool QnFfmpegVideoDecoder::hardwareDecoder() const
{
    return false;
}

void QnFfmpegVideoDecoder::setLightCpuMode(QnAbstractVideoDecoder::DecodeMode val)
{
    if (m_decodeMode == val)
        return;
    if (val >= m_decodeMode || m_decodeMode < DecodeMode_Fastest)
    {
        m_decodeMode = val;
        m_newDecodeMode = DecodeMode_NotDefined;
        m_lightModeFrameCounter = 0;
    }
    else
    {
        m_newDecodeMode = val;
    }
}

AVCodecContext* QnFfmpegVideoDecoder::getContext() const
{
    return m_context;
}

void QnFfmpegVideoDecoder::setMultiThreadDecodePolicy(MultiThreadDecodePolicy value)
{
    if (m_mtDecodingPolicy != value)
    {
        NX_VERBOSE(this, nx::format("Use multithreaded decoding policy %1").arg((int) value));
        m_mtDecodingPolicy = value;
        setMultiThreadDecoding(m_mtDecodingPolicy == MultiThreadDecodePolicy::enabled);
    }
}
