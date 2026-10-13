// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "avi_archive_delegate.h"

#include <chrono>

#include <stdint.h>

#include <QtCore/QDataStream>
#include <QtCore/QSharedPointer>

#include <core/resource/resource_media_layout.h>
#include <core/resource/storage_plugin_factory.h>
#include <core/resource/storage_resource.h>
#include <nx/codec/aac/aac.h>
#include <nx/codec/h263/h263_utils.h>
#include <nx/codec/h264/common.h>
#include <nx/codec/nal_units.h>
#include <nx/media/av_codec_helper.h>
#include <nx/media/codec_parameters.h>
#include <nx/media/config.h>
#include <nx/media/ffmpeg/av_packet.h>
#include <nx/media/ffmpeg/ffmpeg_utils.h>
#include <nx/media/ffmpeg_helper.h>
#include <nx/media/meta_data_packet.h>
#include <nx/media/video_data_packet.h>
#include <nx/utils/log/log.h>
#include <nx/vms/api/data/storage_init_result.h>
#include <nx/vms/common/application_context.h>
#include <utils/common/util.h>
#include <utils/media/ffmpeg_io_context.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
} // extern "C"

using namespace nx::vms::common;

namespace {

// Try to reopen file if seek was faulty and seek time doesn't exceeds this constant.
static const std::chrono::microseconds kMaxFaultySeekReopenOffset = std::chrono::seconds(15);
static const qint64 kSeekError = -1;

/**
 * Parses h264 NAL unit list to determine if it contains I frame (IDR or non-IDR).
 * Since we may have multiple NAL units without actual data (such as SPS, PPS), we have to iterate
 * them until we meet the one with data to check if it's a I frame. That might be time-consuming,
 * so this function should not be used after at least one key frame has been found.
 */
static bool isH264IFrame(const QnAbstractMediaDataPtr& data)
{
    if (data->context->getCodecId() != AV_CODEC_ID_H264)
        return false;

    const quint8* pData = (const quint8*) data->data();
    const quint8* pDataEnd = pData + data->dataSize();
    while ((pData = nx::media::nal::findNextNAL(pData, pDataEnd)) != pDataEnd)
    {
        if (nx::media::h264::isIFrame(pData, pDataEnd - pData))
            return true;
    }

    return false;
}

/**
 * Converts a value in the time base m_firstDts uses - the video stream's, or the
 * first stream's when the file carries no video - into microseconds. Every stream
 * of an MPEG-TS file shares one time base, so either basis gives the same answer.
 */
static qint64 firstDtsToUs(const AVFormatContext* context, int videoIndex, qint64 ticks)
{
    if (!context || context->nb_streams == 0 || ticks == 0)
        return 0;

    const int index =
        (videoIndex >= 0 && (unsigned int) videoIndex < context->nb_streams) ? videoIndex : 0;
    return av_rescale_q(ticks, context->streams[index]->time_base, AVRational{1, 1000000});
}

static void updateKeyFrameFlagH263(QnAbstractMediaDataPtr& data)
{
    nx::media::h263::PictureHeader header;
    if (header.decode((uint8_t*) data->data(), data->dataSize()))
    {
        data->flags.setFlag(
            QnAbstractMediaData::MediaFlag::MediaFlags_AVKey,
            nx::media::h263::isKeyFrame(header.pictureType));
    }
}

// Data-stream codecs the delegate surfaces: KLV analytics metadata and the
// BIN_DATA archive encryption key tag. Other data streams are skipped.
static bool isSupportedDataCodec(AVCodecID codecId)
{
    return codecId == AV_CODEC_ID_SMPTE_KLV || codecId == AV_CODEC_ID_BIN_DATA;
}

} // namespace

QnAviArchiveDelegate::QnAviArchiveDelegate():
    m_openMutex(nx::Mutex::Recursive)
{
    m_flags |= Flag_CanSendMetadata;
}

void QnAviArchiveDelegate::setUseAbsolutePos(bool value)
{
    m_useAbsolutePos = value;
}

QnAviArchiveDelegate::~QnAviArchiveDelegate()
{
    close();
}

qint64 QnAviArchiveDelegate::startTime() const
{
    return m_startTimeUs;
}

void QnAviArchiveDelegate::setStartTimeUs(qint64 startTimeUs)
{
    m_startTimeUs = startTimeUs;
}

qint64 QnAviArchiveDelegate::endTime() const
{
    if (m_durationUs == qint64(AV_NOPTS_VALUE))
        return m_durationUs;
    else
        return m_durationUs + m_startTimeUs;
}

CodecParametersConstPtr QnAviArchiveDelegate::getCodecContext(AVStream* stream)
{
    while (m_contexts.size() <= stream->index)
        m_contexts << CodecParametersConstPtr(nullptr);

    if (m_contexts[stream->index] == 0
        || m_contexts[stream->index]->getCodecId() != stream->codecpar->codec_id)
    {
        m_contexts[stream->index] = std::make_shared<CodecParameters>(stream->codecpar);
    }

    return m_contexts[stream->index];
}

void QnAviArchiveDelegate::fillAudioParamsFromPacket(AVStream* stream, const AVPacket* packet)
{
    if (!stream || !packet || stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
        return; //< Not audio.

    AVCodecParameters* codecpar = stream->codecpar;

    // The params are already known on the normal open path.
    const bool needParams = codecpar->sample_rate <= 0;

    // MPEG-TS carries AAC as ADTS, whose configuration is in-band, so the demuxer
    // leaves extradata empty even after a full find_stream_info. The MP4 muxer
    // needs that configuration up front as an AudioSpecificConfig blob, so a
    // direct stream copy of archive audio into MP4 cannot open without it. The
    // ADTS header carries everything the ASC needs, so build it here.
    const bool needExtradata = codecpar->codec_id == AV_CODEC_ID_AAC
        && codecpar->extradata_size == 0 && !codecpar->extradata;

    if (!needParams && !needExtradata)
        return;

    if (codecpar->codec_id == AV_CODEC_ID_AAC)
    {
        bool filled = false;
        if (needParams)
        {
            // FFmpeg's AAC parser deliberately refuses to report sample_rate/channels
            // (with HE-AAC, SBR doubles the rate and parametric stereo changes the
            // channel count, so the ADTS header alone is ambiguous - see libavcodec
            // aac_ac3_parser.c), and a decoder needs a multi-packet run. Parse the ADTS
            // header directly with our helper instead - the same values the AAC RTP
            // parser feeds to CodecParameters.
            AACCodec aac;
            if (aac.decodeFrame(packet->data, packet->data + packet->size)
                && aac.m_sample_rate > 0)
            {
                codecpar->sample_rate = aac.m_sample_rate;
                av_channel_layout_default(&codecpar->ch_layout, aac.m_channels);
                codecpar->format = AV_SAMPLE_FMT_FLTP; //< AAC decodes to planar float.
                filled = true;
            }
        }

        if (needExtradata)
        {
            AdtsHeader adts;
            QByteArray asc;
            if (adts.decodeFromFrame(packet->data, packet->size)
                && adts.encodeToFfmpegExtradata(&asc) && !asc.isEmpty())
            {
                // Freed by avcodec_parameters_free, so it must be av_malloc'ed.
                auto* extradata = (uint8_t*) av_mallocz(asc.size() + AV_INPUT_BUFFER_PADDING_SIZE);
                if (extradata)
                {
                    memcpy(extradata, asc.constData(), asc.size());
                    codecpar->extradata = extradata;
                    codecpar->extradata_size = (int) asc.size();
                    filled = true;
                }
            }
        }

        if (filled && stream->index < m_contexts.size())
            m_contexts[stream->index] = CodecParametersConstPtr(nullptr);
        return;
    }

    if (!needParams)
        return;

    // Other audio codecs (AC-3, DTS, ...): their parser does fill sample_rate and
    // channel layout from the frame header.
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    AVCodecParserContext* parser = av_parser_init(codecpar->codec_id);
    AVCodecContext* context = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (codec && parser && context)
    {
        uint8_t* outData = nullptr;
        int outSize = 0;
        av_parser_parse2(
            parser,
            context,
            &outData,
            &outSize,
            packet->data,
            packet->size,
            AV_NOPTS_VALUE,
            AV_NOPTS_VALUE,
            0);
        if (context->sample_rate > 0)
        {
            codecpar->sample_rate = context->sample_rate;
            av_channel_layout_copy(&codecpar->ch_layout, &context->ch_layout);
            if (codecpar->format == AV_SAMPLE_FMT_NONE && codec->sample_fmts)
                codecpar->format = codec->sample_fmts[0];
            if (stream->index < m_contexts.size())
                m_contexts[stream->index] = CodecParametersConstPtr(nullptr);
        }
    }
    if (context)
        avcodec_free_context(&context);
    if (parser)
        av_parser_close(parser);
}

bool QnAviArchiveDelegate::readFailed() const
{
    return m_readFailed;
}

QnAbstractMediaDataPtr QnAviArchiveDelegate::getNextData()
{
    if (!m_streamsFound || m_eofReached)
        return QnAbstractMediaDataPtr();

    QnAbstractMediaDataPtr data;
    AVStream* stream;
    while (1)
    {
        nx::media::ffmpeg::AvPacket avPacket;
        auto packet = avPacket.get();
        double time_base;
        int status = av_read_frame(m_formatContext, packet);
        if (status < 0)
        {
            if (status != AVERROR_EOF)
            {
                NX_DEBUG(
                    this,
                    "Failed to read frame: %1",
                    nx::media::ffmpeg::avErrorToString(status));
                m_readFailed = true;
            }
            return QnAbstractMediaDataPtr();
        }

        stream = m_formatContext->streams[packet->stream_index];
        if (stream->codecpar->codec_id == AV_CODEC_ID_H264 && packet->size == 6)
        {
            // may be H264 delimiter as separate packet. remove it
            if (packet->data[0] == 0x00 && packet->data[1] == 0x00 && packet->data[2] == 0x00
                && packet->data[3] == 0x01 && packet->data[4] == nx::media::h264::nuDelimiter)
                continue; // skip delimiter
        }

        if (m_indexToChannel.empty())
            initLayoutStreams();

        auto indexToChannelIt = m_indexToChannel.find(packet->stream_index);
        if (indexToChannelIt == m_indexToChannel.cend())
        {
            NX_DEBUG(
                this,
                "Skip packet, stream number: %1, overall streams: %2",
                packet->stream_index,
                m_indexToChannel.size());
            continue;
        }

        switch (stream->codecpar->codec_type)
        {
            case AVMEDIA_TYPE_VIDEO:
            {
                auto videoData =
                    new QnWritableCompressedVideoData(packet->size, getCodecContext(stream));
                videoData->channelNumber = indexToChannelIt->second;
                if (stream->codecpar->width > 16 && stream->codecpar->height > 16)
                {
                    videoData->width = stream->codecpar->width;
                    videoData->height = stream->codecpar->height;
                }
                data = QnAbstractMediaDataPtr(videoData);
                packetTimestamp(videoData, *packet);
                videoData->m_data.write((const char*) packet->data, packet->size);
                break;
            }

            case AVMEDIA_TYPE_AUDIO:
            {
                if (packet->stream_index != m_audioStreamIndex)
                    continue;

                qint64 timestamp = packetTimestamp(*packet);
                if (!hasVideo() && m_lastSeekTime != AV_NOPTS_VALUE && timestamp < m_lastSeekTime)
                    continue; // seek is broken for audio only media streams

                // Fast-open mode may leave the audio codecpar without
                // sample_rate/ch_layout; recover them from this packet so the
                // context handed downstream is complete.
                fillAudioParamsFromPacket(stream, packet);

                QnWritableCompressedAudioData* audioData =
                    new QnWritableCompressedAudioData(packet->size, getCodecContext(stream));
                // audioData->format.fromAvStream(stream->codec);
                time_base = av_q2d(stream->time_base) * 1e+6;
                audioData->duration = qint64(time_base * packet->duration);
                data = QnAbstractMediaDataPtr(audioData);
                audioData->channelNumber = indexToChannelIt->second;
                audioData->timestamp = timestamp;
                audioData->m_data.write((const char*) packet->data, packet->size);
                break;
            }
            case AVMEDIA_TYPE_DATA:
            {
                const auto dataCodec = stream->codecpar->codec_id;
                if (!isSupportedDataCodec(dataCodec))
                    continue;

                auto metadata =
                    std::make_shared<QnCompressedMetadata>(MetadataType::object, packet->size);
                metadata->m_data.write((const char*) packet->data, packet->size);
                metadata->timestamp = packetTimestamp(*packet);
                // Carry the codec so the reader can tell the crypto key-tag PID
                // (BIN_DATA) from the analytics metadata PID (SMPTE_KLV).
                metadata->compressionType = dataCodec;
                data = QnAbstractMediaDataPtr(metadata);
                break;
            }
            default:
            {
                continue;
            }
        }

        if (packet->flags & AV_PKT_FLAG_KEY)
            data->flags |= QnAbstractMediaData::MediaFlags_AVKey;

        while (packet->stream_index >= m_lastPacketTimes.size())
            m_lastPacketTimes << m_startTimeUs;
        if (data->timestamp == AV_NOPTS_VALUE)
            data->timestamp = m_lastPacketTimes[packet->stream_index];
        else
            m_lastPacketTimes[packet->stream_index] = data->timestamp;

        break;
    }

    if (data->dataType != QnAbstractMediaData::METADATA)
        data->compressionType = stream->codecpar->codec_id;

    // ffmpeg sets key frame flag for every received h263 frames (bug?), update flag manually.
    if (data->compressionType == AV_CODEC_ID_H263)
        updateKeyFrameFlagH263(data);

    /**
     * When parsing h264 stream FFMPEG fills MediaFlags_AVKey for IDR frames but won't do it for
     * non-IDR ones. That's why we check for such frames manually, but only one time per file open
     * or per seek.
     */
    if (m_keyFrameFound.size() > data->channelNumber && !m_keyFrameFound[data->channelNumber]
        && (data->flags.testFlag(QnAbstractMediaData::MediaFlag::MediaFlags_AVKey)
            || isH264IFrame(data)))
    {
        data->flags |= QnAbstractMediaData::MediaFlag::MediaFlags_AVKey;
        m_keyFrameFound[data->channelNumber] = true;
    }
    data->encryptionData = m_metadata.encryptionData;
    if (m_metadata.metadataStreamVersion <= QnAviArchiveMetadata::kMetadataStreamVersion_2)
        data->flags |= QnAbstractMediaData::MediaFlags_BomDecoding;
    return data;
}

bool QnAviArchiveDelegate::providesMotionPackets() const
{
    return m_metadata.metadataStreamVersion >= QnAviArchiveMetadata::kMetadataStreamVersion_2;
}

qint64 QnAviArchiveDelegate::seekWithFallback(qint64 time, bool findIFrame)
{
    // TODO: #lbusygin check is it still needed, and remove it(just check on single GOP file)
    auto resultTime = seek(time, findIFrame);
    if (resultTime == kSeekError)
    {
        // Try to reopen stream
        if (std::chrono::microseconds(time) > kMaxFaultySeekReopenOffset)
            return kSeekError;

        if (!reopen())
        {
            NX_DEBUG(
                this,
                "Cannot reopen file after faulty seek. Resource URL: %1",
                nx::utils::url::hidePassword(m_url));

            return kSeekError;
        }
        return time;
    };

    return resultTime;
}

qint64 QnAviArchiveDelegate::seek(qint64 time, bool findIFrame)
{
    if (!m_streamsFound)
        return kSeekError;

    const auto endTime = this->endTime();
    if (endTime != AV_NOPTS_VALUE)
    {
        m_eofReached = time > endTime;
        if (m_eofReached)
            return DATETIME_NOW;
    }

    std::fill(m_keyFrameFound.begin(), m_keyFrameFound.end(), false);
    const auto timeToSeek = qMax(time - m_startTimeUs, 0ll);
    if (m_hasVideo)
    {
        // av_seek_frame wants a position on the file's own timeline, which begins
        // at m_firstDts rather than at zero - either because the caller declared a
        // recording offset (setFirstDtsUs) or because the MPEG-TS muxer slid the
        // file forward to keep timestamps non-negative. packetTimestamp() subtracts
        // m_firstDts on the way out, so a target expressed in emitted time has to
        // be put back onto the file's timeline here. Files whose media starts at
        // zero have m_firstDts == 0 and are unaffected.
        const auto target =
            timeToSeek + firstDtsToUs(m_formatContext, m_firstVideoIndex, m_firstDts);
        const auto result = av_seek_frame(
            m_formatContext,
            /*stream_index*/ -1,
            target,
            findIFrame ? AVSEEK_FLAG_BACKWARD : AVSEEK_FLAG_ANY);

        if (result < 0)
        {
            NX_DEBUG(
                this,
                "Cannot seek into position %1. Resource URL: %2, av_seek_frame result: %3.",
                target,
                nx::utils::url::hidePassword(m_url),
                result);
            return kSeekError;
        }
    }
    else
    {
        // MP3 seek is buggy in the current ffmpeg version.
        if (!reopen())
            return kSeekError;
    }

    m_lastSeekTime = timeToSeek + m_startTimeUs; //< File physical time to UTC time.
    return time;
}

qint64 QnAviArchiveDelegate::seekToByteOffset(qint64 byteOffset)
{
    if (!m_streamsFound)
        return kSeekError;

    std::fill(m_keyFrameFound.begin(), m_keyFrameFound.end(), false);

    const int result =
        av_seek_frame(m_formatContext, /*stream_index*/ -1, byteOffset, AVSEEK_FLAG_BYTE);
    if (result < 0)
    {
        NX_DEBUG(
            this,
            "Cannot byte-seek to %1. Resource URL: %2, av_seek_frame result: %3.",
            byteOffset,
            nx::utils::url::hidePassword(m_url),
            result);
        return kSeekError;
    }

    // Timestamp is unknown until the next packet is read; do not pretend to
    // know it. The pump in QnArchiveStreamReader will pick it up from the
    // first frame returned by getNextData().
    m_lastSeekTime = AV_NOPTS_VALUE;
    m_eofReached = false;
    return 0;
}

void QnAviArchiveDelegate::setFirstDtsUs(qint64 firstDtsUs)
{
    m_declaredFirstDtsUs = firstDtsUs;
    if (!m_formatContext || m_formatContext->nb_streams == 0)
        return;

    // packetTimestamp() subtracts m_firstDts from a packet's own timestamp, so it
    // has to be in that packet's stream time base. Every stream of an MPEG-TS file
    // shares one, so the video stream's (or the first stream's, for audio-only) is
    // the right basis.
    const int index =
        (m_firstVideoIndex >= 0 && (unsigned int) m_firstVideoIndex < m_formatContext->nb_streams)
        ? m_firstVideoIndex
        : 0;
    const AVRational timeBase = m_formatContext->streams[index]->time_base;

    m_firstDts = av_rescale_q(firstDtsUs, AVRational{1, 1000000}, timeBase);
    NX_VERBOSE(
        this,
        "PTS base declared as %1 us (%2 in stream time base)",
        firstDtsUs,
        m_firstDts);
}

bool QnAviArchiveDelegate::reopen()
{
    auto storage = m_storage;
    close();
    m_storage = storage;
    if (!open(m_url))
        return false;

    return true;
}

bool QnAviArchiveDelegate::checkStorage()
{
    if (m_storage)
        return true;

    m_storage = QnStorageResourcePtr(appContext()->storagePluginFactory()->createStorage(m_url));
    return (bool) m_storage;
}

bool QnAviArchiveDelegate::open(const QString& url)
{
    NX_MUTEX_LOCKER lock(&m_openMutex); // need refactor. Now open may be called from UI thread!!!

    m_url = url;
    if (m_formatContext == nullptr)
    {
        m_eofReached = false;
        if (!checkStorage())
            return false;

        if (!m_storage->isFileExists(url))
        {
            using namespace nx::utils::url;
            NX_DEBUG(
                this,
                "File %1 is missing in the storage %2",
                hidePassword(url),
                hidePassword(m_storage->getUrl()));

            return false;
        }

        m_formatContext = avformat_alloc_context();
        NX_ASSERT(m_formatContext != nullptr);
        if (m_formatContext == nullptr)
            return false;

        if (m_fastStreamFind)
        {
            // Persistent AVFormatContext limits that bound how much
            // avformat_open_input and avformat_find_stream_info read (VMS-171).
            //
            // probesize: PAT is in the first TS packet and PMT follows within a
            //   few; the codec type comes from the PMT stream_type and the
            //   keyframe's VPS/SPS/PPS are recovered later by the Annex-B->MP4
            //   converter - so a few KB suffices (64 KB leaves headroom above the
            //   format-probe minimum) vs the default 5 MB.
            // skip_estimate_duration_from_pts: skip the end-of-file duration seek;
            //   the duration is unused (the caller sets the start time and takes
            //   keyframe offsets from the .idx).
            m_formatContext->probesize = 64 * 1024;
            m_formatContext->skip_estimate_duration_from_pts = 1;
        }

        m_IOContext = nx::utils::media::createFfmpegIOContext(m_storage, url, QIODevice::ReadOnly);
        if (!m_IOContext)
        {
            close();
            return false;
        }
        m_formatContext->pb = m_IOContext;
        if (m_beforeOpenInputCallback)
            m_beforeOpenInputCallback(this);
        /**
         * If avformat_open_input() fails, FormatCtx will be freed before avformat_open_input()
         * returns.
         */
        if (avformat_open_input(&m_formatContext, "", 0, 0) < 0)
        {
            close();
            return false;
        }

        if (!initMetadata())
        {
            close();
            return false;
        }
        fillVideoLayout();
    }
    m_keyFrameFound.resize(m_formatContext->nb_streams, false);
    if (!findStreams())
        return false;

    // Default to the first audio track. The stream_index gate in getNextData()
    // exists to pick one track among several, not to mute the file - but leaving
    // the index unset left every consumer that does not call setAudioChannel()
    // silently audio-less. On the archive path that also starved
    // fillAudioParamsFromPacket(), so a fast-opened segment could only report the
    // unfilled container codecpar and the WebRTC session died on it (VMS-191).
    // An explicit setAudioChannel() still overrides this.
    if (m_audioStreamIndex < 0)
        setAudioChannel(0);

    return true;
}

bool QnAviArchiveDelegate::opened() const
{
    return m_formatContext != nullptr;
}

void QnAviArchiveDelegate::close()
{
    if (m_IOContext)
    {
        nx::utils::media::closeFfmpegIOContext(m_IOContext);
        if (m_formatContext)
            m_formatContext->pb = nullptr;
        m_IOContext = nullptr;
    }

    if (m_formatContext)
        avformat_close_input(&m_formatContext);

    m_contexts.clear();
    m_formatContext = nullptr;
    m_streamsFound = false;
    m_storage.clear();
    m_lastPacketTimes.clear();
    m_lastSeekTime = AV_NOPTS_VALUE;
    m_keyFrameFound.clear();
    m_indexToChannel.clear();
}

const char* QnAviArchiveDelegate::getTagValue(const char* tagName)
{
    if (!m_formatContext)
        return 0;
    AVDictionaryEntry* entry = av_dict_get(m_formatContext->metadata, tagName, 0, 0);
    return entry ? entry->value : 0;
}

std::optional<QnAviArchiveMetadata> QnAviArchiveDelegate::metadata() const
{
    return m_metadata;
}

bool QnAviArchiveDelegate::hasVideo() const
{
    return m_hasVideo;
}

static QnConstResourceVideoLayoutPtr sDefaultVideoLayout(new QnDefaultResourceVideoLayout());

QnConstResourceVideoLayoutPtr QnAviArchiveDelegate::getVideoLayout()
{
    if (!m_videoLayout)
        return sDefaultVideoLayout;

    return m_videoLayout;
}

void QnAviArchiveDelegate::fillVideoLayout()
{
    m_videoLayout.reset(new QnCustomResourceVideoLayout(QSize(1, 1)));

    if (!m_metadata.videoLayoutSize.isEmpty())
    {
        m_videoLayout->setSize(m_metadata.videoLayoutSize);
        m_videoLayout->setChannels(m_metadata.videoLayoutChannels);
    }

    if (m_useAbsolutePos)
    {
        m_startTimeUs = 1000ll * m_metadata.startTimeMs;
    }
}

AudioLayoutConstPtr QnAviArchiveDelegate::getAudioLayout()
{
    return m_audioLayout;
}

extern "C" {
int interruptDetailFindStreamInfo(void*)
{
    return 1;
}
};

bool QnAviArchiveDelegate::findStreams()
{
    if (!m_formatContext)
        return false;

    if (m_streamsFound)
        return true;

    // The open-time probe limits (probesize, skip_estimate_duration_from_pts) are
    // set in open() before avformat_open_input. find_stream_info gets its own brake
    // here: the interrupt callback aborts its packet-reading analysis loop right
    // away, keeping whatever streams avformat_open_input's probe already registered
    // (codec params it leaves unfilled are recovered later, per-packet). Scoped -
    // set here, cleared immediately after - so it never touches the frame reads
    // that follow. Measured worth on H.265+AAC segments: ~20 ms -> ~1 ms.
    if (m_fastStreamFind)
    {
        m_formatContext->interrupt_callback.callback = &interruptDetailFindStreamInfo;
        avformat_find_stream_info(m_formatContext, nullptr);
        m_formatContext->interrupt_callback.callback = 0;
        m_streamsFound = m_formatContext->nb_streams > 0;
    }
    else
    {
        m_streamsFound = avformat_find_stream_info(m_formatContext, nullptr) >= 0;
    }

    // A base declared through setFirstDtsUs is authoritative, so leave m_firstDts
    // alone: findStreams() runs again on every reopen() - which seek() performs on
    // a file without video, and seekWithFallback() after a faulty seek - and would
    // otherwise discard the declaration and go back to guessing from start_time.
    const bool deriveFirstDts = m_declaredFirstDtsUs == qint64(AV_NOPTS_VALUE);
    if (deriveFirstDts)
        m_firstDts = 0;

    if (m_streamsFound)
    {
        m_durationUs = m_formatContext->duration;
        initLayoutStreams();
        if (deriveFirstDts)
        {
            if (m_firstVideoIndex >= 0)
                m_firstDts = m_formatContext->streams[m_firstVideoIndex]->start_time;
            if (m_firstDts == qint64(AV_NOPTS_VALUE))
                m_firstDts = 0;
        }

        if (m_formatContext->duration_estimation_method == AVFMT_DURATION_FROM_BITRATE)
            NX_DEBUG(this, "File duration can be not accurate!");
    }
    else
    {
        close();
    }
    return m_streamsFound;
}

void QnAviArchiveDelegate::setForcedVideoChannelsCount(int videoChannels)
{
    m_forceVideoChannelsCount = videoChannels;
}

void QnAviArchiveDelegate::initLayoutStreams()
{
    int lastStreamID = -1;
    m_firstVideoIndex = -1;
    m_hasVideo = false;

    std::set<int> videoStreams;
    std::set<int> audioStreams;
    std::set<int> dataStreams;
    for (unsigned i = 0; i < m_formatContext->nb_streams; i++)
    {
        AVStream* strm = m_formatContext->streams[i];
        if (strm->codecpar->codec_type >= AVMEDIA_TYPE_NB)
            continue;

        if (strm->id && strm->id == lastStreamID)
            continue; // duplicate
        lastStreamID = strm->id;

        switch (strm->codecpar->codec_type)
        {
            case AVMEDIA_TYPE_VIDEO:
                if (m_firstVideoIndex == -1)
                    m_firstVideoIndex = i;

                // Ffmpeg has a bug when mux AAC into mkv, see
                // https://github.com/HandBrake/HandBrake/issues/2809.
                // So force video channels count from resource video layout.
                if (m_forceVideoChannelsCount.has_value()
                    && m_forceVideoChannelsCount <= videoStreams.size())
                    continue;
                videoStreams.insert(i);
                m_hasVideo = true;
                break;
            case AVMEDIA_TYPE_AUDIO:
                audioStreams.insert(i);
                break;

            case AVMEDIA_TYPE_DATA:
                if (isSupportedDataCodec(strm->codecpar->codec_id))
                    dataStreams.insert(i);
                break;
            default:
                break;
        }
    }
    int channel = 0;
    for (auto& streamIndex: videoStreams)
        m_indexToChannel[streamIndex] = channel++;
    for (auto& streamIndex: audioStreams)
        m_indexToChannel[streamIndex] = channel++;
    for (auto& streamIndex: dataStreams)
        m_indexToChannel[streamIndex] = channel++;

    m_audioLayout.reset(new AudioLayout(m_formatContext));
}

bool QnAviArchiveDelegate::initMetadata()
{
    return true;
}

qint64 QnAviArchiveDelegate::packetTimestamp(const AVPacket& packet)
{
    AVStream* stream = m_formatContext->streams[packet.stream_index];
    double timeBase = av_q2d(stream->time_base) * 1'000'000;
    qint64 packetTime = packet.dts != qint64(AV_NOPTS_VALUE) ? packet.dts : packet.pts;
    if (packetTime == qint64(AV_NOPTS_VALUE))
        return AV_NOPTS_VALUE;
    else
        return qMax(0ll, (qint64) (timeBase * (packetTime - m_firstDts))) + m_startTimeUs;
}

void QnAviArchiveDelegate::packetTimestamp(QnCompressedVideoData* video, const AVPacket& packet)
{
    AVStream* stream = m_formatContext->streams[packet.stream_index];
    double timeBase = av_q2d(stream->time_base) * 1'000'000;
    qint64 packetTime = packet.dts != qint64(AV_NOPTS_VALUE) ? packet.dts : packet.pts;
    if (packetTime == qint64(AV_NOPTS_VALUE))
        video->timestamp = AV_NOPTS_VALUE;
    else
        video->timestamp =
            qMax(0ll, (qint64) (timeBase * (packetTime - m_firstDts))) + m_startTimeUs;

    if (packet.pts != AV_NOPTS_VALUE)
    {
        video->pts = qMax(0ll, (qint64) (timeBase * (packet.pts - m_firstDts))) + m_startTimeUs;
    }
}

bool QnAviArchiveDelegate::setAudioChannel(unsigned num)
{
    if (!m_formatContext)
        return false;

    if (!m_streamsFound)
        return false;

    // Convert num to absolute track number.
    m_audioStreamIndex = -1;
    int lastStreamID = -1;
    unsigned currentAudioTrackNum = 0;

    for (unsigned i = 0; i < m_formatContext->nb_streams; i++)
    {
        const AVStream* strm = m_formatContext->streams[i];

        if (strm->codecpar->codec_type >= AVMEDIA_TYPE_NB)
            continue;

        if (strm->id && strm->id == lastStreamID)
            continue; //< Duplicate.

        lastStreamID = strm->id;

        if (strm->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            if (currentAudioTrackNum == num)
            {
                m_audioStreamIndex = (int) i;
                m_selectedAudioChannel = num;
                return m_formatContext->streams[m_audioStreamIndex]->codecpar != nullptr;
            }
            currentAudioTrackNum++;
        }
    }
    return false;
}

bool QnAviArchiveDelegate::isStreamsFound() const
{
    return m_streamsFound;
}

void QnAviArchiveDelegate::setStorage(const QnStorageResourcePtr& storage)
{
    m_storage = storage;
}

void QnAviArchiveDelegate::setFastStreamFind(bool value)
{
    m_fastStreamFind = value;
}

QIODevice* QnAviArchiveDelegate::ioDevice() const
{
    return m_IOContext ? (QIODevice*) m_IOContext->opaque : nullptr;
}

void QnAviArchiveDelegate::setBeforeOpenInputCallback(BeforeOpenInputCallback callback)
{
    m_beforeOpenInputCallback = callback;
}
