// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "ffmpeg_muxer.h"

extern "C" {
#include <libavutil/opt.h>
}

#include <media/filters/abstract_media_data_filter.h>
#include <nx/codec/aac/aac.h>
#include <nx/media/ffmpeg/av_packet.h>
#include <nx/media/ffmpeg/ffmpeg_utils.h>
#include <nx/media/utils.h>
#include <nx/utils/log/log.h>
#include <utils/common/util.h>

namespace {

/**
 * Drops the ADTS header from an AAC frame bound for a moov-based container.
 * MPEG-TS carries AAC as ADTS, a 7 or 9 byte per-frame header repeating the
 * configuration in-band. Moov containers store that configuration once, in
 * extradata, and reject framed payloads outright ("Malformed AAC bitstream
 * detected"), so a direct stream copy out of the archive has to strip it. This
 * is the packet half of what ffmpeg's `aac_adtstoasc` filter does - the
 * extradata half is built in QnAviArchiveDelegate::fillAudioParamsFromPacket.
 */
void stripAdtsHeaderForMoovIfNeeded(
    const QString& container,
    const AVCodecParameters* audioCodecParameters,
    const QnConstAbstractMediaDataPtr& media,
    AVPacket* packet)
{
    if (media->dataType != QnAbstractMediaData::AUDIO || !audioCodecParameters
        || audioCodecParameters->codec_id != AV_CODEC_ID_AAC
        || !FfmpegMuxer::isMoovFormat(container))
    {
        return;
    }

    AdtsHeader adts;
    if (!adts.decodeFromFrame(packet->data, packet->size))
        return; //< Already raw AAC.

    const int headerLength = adts.length();
    if (packet->size <= headerLength)
        return;

    packet->data += headerLength;
    packet->size -= headerLength;
}

static AVPixelFormat jpegPixelFormatToRtp(AVPixelFormat value)
{
    switch (value)
    {
        case AV_PIX_FMT_YUV420P:
            return AV_PIX_FMT_YUVJ420P;
        case AV_PIX_FMT_YUV422P:
            return AV_PIX_FMT_YUVJ422P;
        case AV_PIX_FMT_YUV444P:
            return AV_PIX_FMT_YUVJ444P;
        default:
            return value;
    }
}

static AVPixelFormat getPixelFormatJpeg(const std::string_view container, AVPixelFormat format)
{
    if (container == "rtp")
        return jpegPixelFormatToRtp(format);
    else
        return format;
}

} // namespace

FfmpegMuxer::FfmpegMuxer(const Config& config):
    m_config(config),
    m_internalBuffer(CL_MEDIA_ALIGNMENT, 1024 * 1024, AV_INPUT_BUFFER_PADDING_SIZE),
    m_timestampCorrector(
        std::chrono::milliseconds(MAX_FRAME_DURATION_MS),
        std::chrono::milliseconds(1))
{
}

FfmpegMuxer::~FfmpegMuxer()
{
    closeFfmpegContext();
}

bool FfmpegMuxer::isCodecSupported(AVCodecID id) const
{
    if (!m_formatCtx || !m_formatCtx->oformat)
        return false;

    return avformat_query_codec(m_formatCtx->oformat, id, FF_COMPLIANCE_NORMAL) == 1;
}

bool FfmpegMuxer::process(const QnConstAbstractMediaDataPtr& media)
{
    m_outputPacketSize.clear();
    if (media->dataType != QnAbstractMediaData::VIDEO
        && media->dataType != QnAbstractMediaData::AUDIO)
        return true; // mux only audio and video, skip packet

    if ((media->dataType == QnAbstractMediaData::AUDIO && !m_initializedAudio)
        || (media->dataType == QnAbstractMediaData::VIDEO && !m_initializedVideo))
    {
        return true;
    }

    return muxPacket(media);
}

void FfmpegMuxer::setPacketizedMode(bool value)
{
    m_packetizedMode = value;
}

const QList<int>& FfmpegMuxer::getPacketsSize()
{
    return m_outputPacketSize;
}

void FfmpegMuxer::closeFfmpegContext()
{
    if (m_formatCtx)
    {
        if (m_initialized)
            av_write_trailer(m_formatCtx);
        m_initialized = false;
        m_ioContext.reset();
        m_formatCtx->pb = nullptr;
        m_audioCodecParameters = nullptr;
        m_videoCodecParameters = nullptr;
        avformat_close_input(&m_formatCtx);
    }
}

bool FfmpegMuxer::setContainer(const QString& container)
{
    m_container = container;
    const AVOutputFormat* outputCtx = av_guess_format(m_container.toLatin1().data(), NULL, NULL);
    if (outputCtx == 0)
    {
        NX_WARNING(this, "Container %1 was not found in FFMPEG library.", container);
        return false;
    }

    int err = avformat_alloc_output_context2(&m_formatCtx, outputCtx, 0, "");
    if (err != 0)
    {
        NX_WARNING(this, "Could not create output context for format %1.", container);
        return false;
    }
    if (container == QLatin1String("rtp"))
        m_formatCtx->packet_size = m_rtpMtu;

    return true;
}

void FfmpegMuxer::setFormatOption(const QString& option, const QString& value)
{
    av_opt_set(m_formatCtx->priv_data, option.toLatin1().data(), value.toLatin1().data(), 0);
}

bool FfmpegMuxer::addVideo(const AVCodecParameters* codecParameters)
{
    if (!m_formatCtx || !codecParameters)
        return false;

    NX_DEBUG(this, "Add video %1.", codecParameters->codec_id);
    AVStream* videoStream = avformat_new_stream(m_formatCtx, nullptr);
    if (videoStream == 0)
    {
        NX_ERROR(this, "Could not allocate output stream for recording.");
        return false;
    }

    m_videoCodecParameters = videoStream->codecpar;
    avcodec_parameters_copy(m_videoCodecParameters, codecParameters);
    m_videoCodecParameters->codec_tag = 0; //< Zero tag since mp4 does not support tags.

    if (m_videoCodecParameters->codec_id == AV_CODEC_ID_MJPEG)
    {
        m_videoCodecParameters->format = getPixelFormatJpeg(
            m_container.toStdString(),
            (AVPixelFormat) m_videoCodecParameters->format);
    }

    m_videoCodecParameters->sample_aspect_ratio.num = 1;
    m_videoCodecParameters->sample_aspect_ratio.den = 1;

    videoStream->id = 0;
    videoStream->time_base.num = 1;
    videoStream->time_base.den = 60;
    videoStream->sample_aspect_ratio = m_videoCodecParameters->sample_aspect_ratio;
    m_initializedVideo = true;
    return true;
}

bool FfmpegMuxer::isMoovFormat(const QString& container)
{
    return container.compare("mp4", Qt::CaseInsensitive) == 0
        || container.compare("ismv", Qt::CaseInsensitive) == 0;
}

bool FfmpegMuxer::addAudio(const AVCodecParameters* codecParameters)
{
    if (!m_formatCtx || !codecParameters)
        return false;

    NX_DEBUG(this, "Add audio %1.", codecParameters->codec_id);
    AVStream* audioStream = avformat_new_stream(m_formatCtx, nullptr);
    if (audioStream == 0)
    {
        NX_ERROR(this, "Could not allocate output stream for recording.");
        return false;
    }

    audioStream->id = 0;
    m_audioCodecParameters = audioStream->codecpar;
    avcodec_parameters_copy(m_audioCodecParameters, codecParameters);
    // Zero tag, as addVideo does: a direct stream copy carries the source
    // container's tag, which the output container rejects. MPEG-TS archive audio
    // arrives tagged with the stream type (15 for ADTS AAC) and mp4 wants mp4a,
    // so without this every export of a segment with audio fails to open with
    // "Tag [15][0][0][0] incompatible with output codec id".
    m_audioCodecParameters->codec_tag = 0;
    m_initializedAudio = true;
    return true;
}

bool FfmpegMuxer::open(std::optional<QnAviArchiveMetadata> metadata)
{
    if (m_initialized)
    {
        NX_WARNING(this, "Double initialization");
        return false;
    }

    if (m_formatCtx->nb_streams == 0)
        return false;

    m_ioContext = std::make_unique<nx::media::ffmpeg::IoContext>(
        /*bufferSize*/ 1024 * 16,
        /*writable*/ true,
        /*seekable*/ false);
    m_ioContext->writeHandler = [this](const uint8_t* data, int size)
    {
        m_internalBuffer.write((const char*) data, size);
        if (m_packetizedMode)
            m_outputPacketSize << size;
        return size;
    };

    m_formatCtx->pb = m_ioContext->getAvioContext();
    if (metadata)
        metadata->saveToFile(m_formatCtx, metadata->formatFromExtension(m_container));
    int rez = avformat_write_header(m_formatCtx, 0);
    if (rez < 0)
    {
        closeFfmpegContext();
        NX_ERROR(this, "Video or audio codec is incompatible with container %1.", m_container);
        return false;
    }
    m_initialized = true;
    return true;
}

bool FfmpegMuxer::addTag(const char* name, const char* value)
{
    return av_dict_set(&m_formatCtx->metadata, name, value, 0) >= 0;
}

void FfmpegMuxer::setStartTimeOffset(int64_t value)
{
    m_timestampCorrector.setOffset(std::chrono::microseconds(value));
}

void FfmpegMuxer::rebaseOutputTimestamps(std::chrono::microseconds baseUs)
{
    // The base is client-chosen and 32-bit on the wire, so any value is legal - but ffmpeg
    // refuses output DTS going backwards, and a base below the written timeline froze the
    // stream until the media caught up (a player picks a low base when it saw no frames to
    // raise it, e.g. going live on an offline camera). Lift such a base by whole 2^32
    // periods of the 90 kHz RTP clock: the 32-bit wire value the player gates on is
    // unchanged, and the 64-bit timeline keeps advancing.
    constexpr auto kRtpWrapUs = std::chrono::microseconds(((int64_t(1) << 32) * 100 + 8) / 9);
    if (baseUs.count() <= m_lastOutputTimestampUs)
    {
        const int64_t periods =
            (m_lastOutputTimestampUs - baseUs.count()) / kRtpWrapUs.count() + 1;
        baseUs += periods * kRtpWrapUs;
    }

    // After clear() the next processed packet defines the corrector base time,
    // so its output is exactly the offset.
    m_timestampCorrector.clear();
    m_timestampCorrector.setOffset(baseUs);
}

bool FfmpegMuxer::muxPacket(QnConstAbstractMediaDataPtr media)
{
    int streamIndex = 0;
    if (m_initializedVideo && media->dataType == QnAbstractMediaData::AUDIO)
        streamIndex = 1;

    if (streamIndex >= (int) m_formatCtx->nb_streams)
    {
        NX_DEBUG(this, "Invalid packet media type: %1, skip it", media->dataType);
        return true;
    }

    if (!m_firstVideoTimestamp && media->dataType == QnAbstractMediaData::VIDEO)
        m_firstVideoTimestamp = media->timestamp;

    if (m_initializedVideo && media->dataType == QnAbstractMediaData::AUDIO
        && (!m_firstVideoTimestamp || m_firstVideoTimestamp > media->timestamp))
    {
        NX_DEBUG(
            this,
            "Skip audio before video: %1, first video timestamp: %2",
            media,
            m_firstVideoTimestamp ? *m_firstVideoTimestamp : 0);
        return true;
    }

    if (m_videoStreamFilter && media->dataType == QnAbstractMediaData::VIDEO)
        media = std::dynamic_pointer_cast<const QnAbstractMediaData>(
            m_videoStreamFilter->processData(media));

    AVStream* stream = m_formatCtx->streams[streamIndex];
    constexpr AVRational srcRate = {1, 1'000'000};
    nx::media::ffmpeg::AvPacket avPacket;
    auto packet = avPacket.get();

    bool allowEqualTimestamps = media->dataType == QnAbstractMediaData::METADATA;
    // Force a timestamp discontinuity for three cases:
    //  - MediaFlags_Discontinuity: explicit upstream signal.
    //  - MediaFlags_BOF: the first key frame after every seek (set by
    //    QnArchiveStreamReader). Without this, small forward seeks
    //    (< MAX_FRAME_DURATION_MS == 1.1 s) are not auto-detected by the
    //    corrector, the output PTS keeps the forward gap, and the receiving
    //    browser holds the last decoded frame on screen for the gap duration —
    //    visible as a "freeze for ~500 ms" after a seek.
    //  - MediaFlags_PreRoll: a pre-roll frame from the seek I-frame up to and
    //    including the seek target (the last pre-roll frame). The corrector
    //    turns each discontinuity into "lastTimestamp + 1 ms", which is exactly
    //    what we want here: every pre-roll frame collapses to a 1 ms PTS step so
    //    the browser decodes them at maximum rate, the user perceives the seek
    //    as landing at the requested position, and the wall-clock label (carried
    //    separately via abs-capture-time) stays correct. The first frame past the
    //    pre-roll run is not forced, so it resumes natural cadence.
    const bool forceDisc = media->flags
        & (QnAbstractMediaData::MediaFlags_Discontinuity | QnAbstractMediaData::MediaFlags_BOF
           | QnAbstractMediaData::MediaFlags_PreRoll);
    auto timestamp = m_config.utcTimestamps ? media->timestamp
                                            : m_timestampCorrector
                                                  .process(
                                                      std::chrono::microseconds(media->timestamp),
                                                      streamIndex,
                                                      allowEqualTimestamps,
                                                      forceDisc)
                                                  .count();

    packet->pts = av_rescale_q(timestamp, srcRate, stream->time_base);

    packet->data = (uint8_t*) media->data();
    packet->size = static_cast<int>(media->dataSize());
    stripAdtsHeaderForMoovIfNeeded(m_container, m_audioCodecParameters, media, packet);
    if (media->dataType == QnAbstractMediaData::AUDIO
        || media->flags & QnAbstractMediaData::MediaFlags_AVKey)
        packet->flags |= AV_PKT_FLAG_KEY;

    packet->stream_index = streamIndex;
    packet->dts = packet->pts;

    m_lastPacketTimestamp.ntpTimestamp = media->timestamp;
    m_lastPacketTimestamp.rtpTimestamp = packet->pts;

    int status = av_write_frame(m_formatCtx, packet);
    if (status < 0)
    {
        NX_WARNING(
            this,
            "Muxing packet error: can't write AV packet: %1, error: %2",
            media,
            nx::media::ffmpeg::avErrorToString(status));
        return false;
    }
    // The written timeline's high-water mark; rebaseOutputTimestamps() must never move the
    // output below it.
    m_lastOutputTimestampUs = std::max<int64_t>(m_lastOutputTimestampUs, timestamp);

    if (m_config.computeSignature)
    {
        // TODO: not implemented
    }
    return true;
}

bool FfmpegMuxer::finalize()
{
    closeFfmpegContext();
    return true;
}

void FfmpegMuxer::getResult(nx::utils::ByteArray* const result, bool clearResult)
{
    if (result)
    {
        if (clearResult)
            result->clear();
        result->write(m_internalBuffer.data(), m_internalBuffer.size());
    }
    m_internalBuffer.clear();
}

void FfmpegMuxer::getResult(nx::Buffer* result, bool clearResult)
{
    if (result)
    {
        if (clearResult)
            result->clear();
        result->append(m_internalBuffer.data(), static_cast<int>(m_internalBuffer.size()));
    }
    m_internalBuffer.clear();
}

AVCodecParameters* FfmpegMuxer::getVideoCodecParameters() const
{
    return m_videoCodecParameters;
}

AVCodecParameters* FfmpegMuxer::getAudioCodecParameters() const
{
    return m_audioCodecParameters;
}

void FfmpegMuxer::setVideoStreamFilter(std::unique_ptr<AbstractMediaDataFilter> filter)
{
    m_videoStreamFilter = std::move(filter);
}
