// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "rtsp_stream_provider.h"

#include <set>

#include <QtCore/QSettings>

#include <core/resource/resource_media_layout.h>
#include <nx/network/compat_poll.h>
#include <nx/network/rtsp/rtsp_types.h>
#include <nx/reflect/string_conversion.h>
#include <nx/rtp/parsers/aac_rtp_parser.h>
#include <nx/rtp/parsers/h264_rtp_parser.h>
#include <nx/rtp/parsers/hevc_rtp_parser.h>
#include <nx/rtp/parsers/i_rtp_parser_factory.h>
#include <nx/rtp/parsers/mjpeg_rtp_parser.h>
#include <nx/rtp/parsers/mpeg12_audio_rtp_parser.h>
#include <nx/rtp/parsers/mpeg4_rtp_parser.h>
#include <nx/rtp/parsers/rtp_parser.h>
#include <nx/rtp/parsers/simpleaudio_rtp_parser.h>
#include <nx/rtp/rtp.h>
#include <nx/streaming/rtp/parsers/nx_rtp_metadata_parser.h>
#include <nx/streaming/rtp/parsers/nx_rtp_parser.h>
#include <nx/utils/log/log.h>
#include <nx/utils/scope_guard.h>
#include <nx/vms/api/types/rtp_types.h>
#include <nx/vms/common/system_context.h>
#include <nx/vms/common/system_settings.h>
#include <utils/common/synctime.h>
#include <utils/common/util.h>

using namespace nx;

namespace {

static const int RTCP_REPORT_TIMEOUT = 30 * 1000;

static const int MAX_MEDIA_SOCKET_COUNT = 5;
static const int MEDIA_DATA_READ_TIMEOUT_MS = 100;

// prefix has the following format $<ChannelId(1byte)><PayloadLength(2bytes)>
static const int kInterleavedRtpOverTcpPrefixLength = 4;

} // namespace

namespace nx::streaming {

nx::vms::api::RtpTransportType RtspStreamProvider::s_defaultTransportToUse =
    nx::vms::api::RtpTransportType::automatic;

RtspStreamProvider::RtspStreamProvider(
    const nx::vms::common::SystemSettings* systemSetting,
    nx::vms::api::StreamIndex streamIndex,
    const nx::streaming::rtp::TimeOffsetPtr& timeOffset,
    bool forceCameraTime):
    m_RtpSession(QnRtspClient::Config{}),
    m_timeOffset(timeOffset),
    m_pleaseStop(false),
    m_gotSomeFrame(false),
    m_gotData(false),
    m_preferredAuthScheme(nx::network::http::header::AuthScheme::digest),
    m_rtpTransport(nx::vms::api::RtpTransportType::automatic),
    m_forceCameraTime(forceCameraTime),
    m_streamIndex(streamIndex)
{
    m_rtpFrameTimeoutMs = systemSetting->rtpFrameTimeoutMs().count();
    m_maxRtpRetryCount = systemSetting->maxRtpRetryCount();

    m_RtpSession.setTCPTimeout(std::chrono::milliseconds(m_rtpFrameTimeoutMs));

    m_numberOfVideoChannels = 1;
    m_customVideoLayout.reset();
}

RtspStreamProvider::~RtspStreamProvider()
{
    for (unsigned int i = 0; i < m_demuxedData.size(); ++i)
        delete m_demuxedData[i];
}

nx::network::MulticastAddressRegistry* RtspStreamProvider::multicastAddressRegistry()
{
    return nullptr;
}

void RtspStreamProvider::setRequest(const QString& request)
{
    m_request = request;
}

void RtspStreamProvider::clearKeyData(int channelNum)
{
    if (m_gotKeyDataInfo.size() > (size_t) channelNum)
        m_gotKeyDataInfo[channelNum] = false;
}

bool RtspStreamProvider::gotKeyData(const QnAbstractMediaDataPtr& mediaData)
{
    if ((size_t) m_gotKeyDataInfo.size() <= mediaData->channelNumber)
    {
        size_t oldSize = m_gotKeyDataInfo.size();
        m_gotKeyDataInfo.resize(mediaData->channelNumber + 1);
        for (size_t i = oldSize; i < m_gotKeyDataInfo.size(); ++i)
            m_gotKeyDataInfo[i] = false;
    }

    if (mediaData->dataType == QnAbstractMediaData::VIDEO)
    {
        if (mediaData->flags & AV_PKT_FLAG_KEY)
            m_gotKeyDataInfo[mediaData->channelNumber] = true;
        return m_gotKeyDataInfo[mediaData->channelNumber];
    }
    return true;
}

QnAbstractMediaDataPtr RtspStreamProvider::getNextData()
{
    NX_VERBOSE(this, "%1: Getting next data", m_logName);
    if (!isStreamOpened())
    {
        NX_VERBOSE(this, "%1: Getting next data, stream is not opened, exiting", m_logName);
        return QnAbstractMediaDataPtr(0);
    }

    PlaybackRange playbackRange;
    {
        NX_MUTEX_LOCKER lock(&m_mutex);
        std::swap(playbackRange, m_playbackRange);
    }

    if (playbackRange.isValid())
    {
        NX_VERBOSE(
            this,
            "%1: Getting next data, seeking to position %2-%3 us",
            m_logName,
            playbackRange.startTimeUsec,
            playbackRange.endTimeUsec);

        m_RtpSession.sendPlay(
            playbackRange.startTimeUsec,
            playbackRange.endTimeUsec,
            m_RtpSession.getScale());

        createTrackParsers();
    }

    QnAbstractMediaDataPtr result;

    m_tcpSocket = m_RtpSession.tcpSock();
    if (!m_tcpSocket)
    {
        NX_DEBUG(this, "%1: Getting next data, stream is not opened, socket is empty", m_logName);
        return nullptr;
    }

    do
    {
        m_dataTimer.restart();
        if (m_RtpSession.getActualTransport() == nx::vms::api::RtpTransportType::tcp)
            result = getNextDataTCP();
        else
            result = getNextDataUDP();

    } while (result && !gotKeyData(result));

    if (result)
    {
        NX_VERBOSE(this, "%1: Got some stream data, dataType: %2", m_logName, result->dataType);

        m_gotSomeFrame = true;
        return result;
    }

    if (m_pleaseStop)
    {
        NX_VERBOSE(this, "%1: RTSP terminated because of pleaseStop call", m_logName);
        return result;
    }

    if (!m_gotSomeFrame)
    {
        NX_VERBOSE(this, "%1: No frame has arrived since the stream was opened", m_logName);
        return result; // if no frame received yet do not report network issue error
    }

    if (m_RtpSession.isOpened())
    {
        if (!m_lastRtpParseResult.success)
        {
            NX_WARNING(this, "Network RTP stream error: %1", m_lastRtpParseResult.message);
        }
        else
        {
            NX_WARNING(
                this,
                "Can not read RTP frame for camera %1 during %2 ms",
                m_logName,
                std::chrono::milliseconds(m_dataTimer.elapsed()));
        }
    }
    else
    {
        NX_WARNING(
            this,
            "%1: RTP connection was forcibly closed by camera. Reopen stream",
            m_logName);
    }

    NX_DEBUG(this, "No video frame for URL %1", m_url);
    return result;
}

void RtspStreamProvider::processTcpRtcp(quint8* buffer, int bufferSize, int bufferCapacity)
{
    if (!m_RtpSession.processTcpRtcpData(buffer, bufferSize))
        NX_VERBOSE(this, "Can't parse RTCP report");
    int outBufSize = nx::rtp::buildClientRtcpReport(buffer + 4, bufferCapacity - 4);
    if (outBufSize > 0)
    {
        quint16* sizeField = (quint16*) (buffer + 2);
        *sizeField = htons(outBufSize);
        m_RtpSession.sendBinaryResponse(buffer, outBufSize + 4);
    }
    m_rtcpReportTimer.restart();
}

void RtspStreamProvider::buildClientRTCPReport(quint8 chNumber)
{
    quint8 buffer[1024 * 4];
    buffer[0] = '$';
    buffer[1] = chNumber;
    int outBufSize = nx::rtp::buildClientRtcpReport(buffer + 4, sizeof(buffer) - 4);
    if (outBufSize > 0)
    {
        quint16* sizeField = (quint16*) (buffer + 2);
        *sizeField = htons(outBufSize);
        m_RtpSession.sendBinaryResponse(buffer, outBufSize + 4);
    }
}

QnAbstractMediaDataPtr RtspStreamProvider::getNextDataInternal()
{
    for (auto& track: m_tracks)
    {
        for (const auto& [payloadType, parser]: track.rtpParsers)
        {
            if (!parser)
                continue;

            nx::rtp::RtcpSenderReport rtcpReport;
            if (!m_ignoreRtcpReports && track.ioDevice)
            {
                rtcpReport = track.ioDevice->getSenderReport();
                if (rtcpReport.ntpTimestamp != 0 && track.timeHelper)
                    track.timeHelper->stopWaitingSenderReport();
            }

            QnAbstractMediaDataPtr result = parser->nextData(rtcpReport);
            if (!result)
                continue;
            if (track.timeHelper)
            {
                // The primary lane's first track is the reference: it owns the shared clock,
                // every other track and the secondary lane bind to it.
                const bool isPrimaryStream = m_streamIndex == nx::vms::api::StreamIndex::primary;
                const bool isPrimaryTrack = &track == &m_tracks.front();
                result->timestamp =
                    track.timeHelper
                        ->getTime(
                            QnSyncTime::currentTimePoint(),
                            std::chrono::microseconds(result->timestamp),
                            parser->isUtcTime(),
                            isPrimaryStream,
                            isPrimaryTrack,
                            [this](nx::streaming::rtp::CameraTimeHelper::EventType event)
                            {
                                processCameraTimeHelperEvent(event);
                            })
                        .count();
            }
            result->channelNumber = track.logicalChannelNum;
            if (result->dataType == QnAbstractMediaData::VIDEO)
            {
                result->channelNumber =
                    std::min(result->channelNumber, (quint32) m_numberOfVideoChannels - 1);
            }
            else if (result->dataType == QnAbstractMediaData::AUDIO && !m_audioLayout)
            {
                m_audioLayout.reset(new AudioLayout(result->context));
            }
            return result;
        }
    }
    return QnAbstractMediaDataPtr();
}

bool RtspStreamProvider::processData(
    TrackInfo& track,
    uint8_t* buffer,
    int offset,
    int size,
    int channel,
    int& errorCount)
{
    bool packetLoss = false;

    const auto payloadType = nx::rtp::RtpHeader::getPayloadType(buffer + offset, size);
    if (!payloadType)
    {
        NX_DEBUG(this, "%1 Invalid RTP header: Unable to extract payload type", m_logName);
        return false;
    }

    const auto it = track.rtpParsers.find(*payloadType);
    if (it == track.rtpParsers.end())
    {
        NX_VERBOSE(this, "%1 Unable to find parser for payload type %2", m_logName, *payloadType);
        return true;
    }

    const auto& parser = it->second;
    auto result = parser->processData(buffer, offset, size, packetLoss, m_gotData);

    if (!result.success)
    {
        NX_DEBUG(this, "%1 RTP parsing error: %2", m_logName, result.message);
        clearKeyData(track.logicalChannelNum);
        m_demuxedData[channel]->clear();
        parser->clear();
    }

    if (!result.success || packetLoss)
    {
        if (++errorCount > m_maxRtpRetryCount)
        {
            m_lastRtpParseResult = result;
            if (result.success && packetLoss)
                m_lastRtpParseResult = {false, "RTP packet loss"};
            NX_WARNING(this, "%1: Too many RTP errors. Report a network issue", m_logName);
            return false;
        }
    }

    if (m_gotData)
        m_demuxedData[channel]->clear();

    return true;
}

bool RtspStreamProvider::isCodecSupportedByCustomParserFactories(const QString& codecName) const
{
    return std::ranges::any_of(
        m_customParserFactories,
        [&codecName](const auto& factory)
        {
            return factory->supportsCodec(codecName);
        });
}

bool RtspStreamProvider::isDataTimerExpired() const
{
    return m_rtpFrameTimeoutMs > 0 && m_dataTimer.elapsed() > m_rtpFrameTimeoutMs;
}

QnAbstractMediaDataPtr RtspStreamProvider::getNextDataTCP()
{
    int errorRetryCnt = 0;
    int rtpChannelNum = -1;

    const auto tcpTimeout = m_RtpSession.getTCPTimeout();
    if (m_callbackTimeout.count() > 0)
        m_RtpSession.setTCPTimeout(m_callbackTimeout);
    const auto scopeGuard = nx::utils::makeScopeGuard(
        [this, tcpTimeout]()
        {
            if (m_callbackTimeout.count() > 0)
                m_RtpSession.setTCPTimeout(tcpTimeout);
        });

    while (m_tcpSocket->isConnected() && !m_pleaseStop && !isDataTimerExpired())
    {
        while (m_gotData)
        {
            QnAbstractMediaDataPtr data = getNextDataInternal();
            if (data)
                return data;
            else
                m_gotData = false;
        }

        m_RtpSession.sendKeepAliveIfNeeded(); //< RTSP GET_PARAMETER
        // RTCP keep-alive packet
        if (m_rtcpReportTimer.elapsed() >= RTCP_REPORT_TIMEOUT)
        {
            for (const TrackInfo& track: m_tracks)
            {
                if (track.ioDevice)
                    buildClientRTCPReport(track.rtcpChannelNumber);
            }
            m_rtcpReportTimer.restart();
        }

        int bytesRead = m_RtpSession.readBinaryResponse(m_demuxedData, rtpChannelNum);
        if (bytesRead < 0
            && !nx::network::socketCannotRecoverFromError(SystemError::getLastOSErrorCode()))
        {
            bool isTimeout = SystemError::getLastOSErrorCode() == SystemError::timedOut
                || SystemError::getLastOSErrorCode() == SystemError::again;
            if (isTimeout && m_onSocketReadTimeoutCallback
                && m_RtpSession.lastReceivedDataTimer().isValid()
                && m_RtpSession.lastReceivedDataTimer().elapsed()
                    < m_RtpSession.sessionTimeoutMs())
            {
                if (auto data = m_onSocketReadTimeoutCallback())
                    return data;
            }
            continue;
        }

        auto trackIndexIter = m_trackIndices.find(rtpChannelNum);
        if (trackIndexIter == m_trackIndices.end() && m_RtpSession.isPlayNowMode())
        {
            registerPredefinedTrack(rtpChannelNum);
            trackIndexIter = m_trackIndices.find(rtpChannelNum);
        }

        if (bytesRead < 1)
        {
            const bool isInvalidTrack = trackIndexIter == m_trackIndices.end();
            const bool isBufferAllocated = (int) m_demuxedData.size() > rtpChannelNum
                && rtpChannelNum >= 0 && m_demuxedData[rtpChannelNum];

            if (isInvalidTrack && isBufferAllocated)
                m_demuxedData[rtpChannelNum]->clear();

            NX_DEBUG(
                this,
                "Failed to read data, bytesRead: %1, rtpChannelNum: %2",
                bytesRead,
                rtpChannelNum);
            break; // error
        }
        NX_VERBOSE(
            this,
            "Got %1 bytes, trackFound=%2",
            bytesRead,
            trackIndexIter != m_trackIndices.end());

        if (m_RtpSession.isRtcp(rtpChannelNum))
        {
            processTcpRtcp(
                (quint8*) m_demuxedData[rtpChannelNum]->data(),
                bytesRead,
                m_demuxedData[rtpChannelNum]->capacity());
            m_demuxedData[rtpChannelNum]->clear();
        }
        else if (trackIndexIter != m_trackIndices.end())
        {
            auto& track = m_tracks[trackIndexIter->second];

            if (m_demuxedData[rtpChannelNum]->size() > MAX_ALLOWED_FRAME_SIZE)
            {
                for (auto& [_, parser]: track.rtpParsers)
                    parser->clear();
                clearKeyData(track.logicalChannelNum);
                m_demuxedData[rtpChannelNum]->clear();
                NX_WARNING(
                    this,
                    "Cleanup data for RTP track %1, URL %2. Buffer overflow",
                    rtpChannelNum,
                    m_url);
                continue;
            }

            const int rtpBufferOffset = m_demuxedData[rtpChannelNum]->size() - bytesRead;
            const int offset = rtpBufferOffset + kInterleavedRtpOverTcpPrefixLength;
            const int length = bytesRead - kInterleavedRtpOverTcpPrefixLength;
            uint8_t* rtpPacketData = (uint8_t*) m_demuxedData[rtpChannelNum]->data();
            if (!processData(track, rtpPacketData, offset, length, rtpChannelNum, errorRetryCnt))
                return QnAbstractMediaDataPtr(0);
        }
        else
        {
            m_demuxedData[rtpChannelNum]->clear();
        }
    }
    return QnAbstractMediaDataPtr();
}

QnAbstractMediaDataPtr RtspStreamProvider::getNextDataUDP()
{
    int errorRetryCount = 0;

    pollfd mediaSockPollArray[MAX_MEDIA_SOCKET_COUNT];
    memset(mediaSockPollArray, 0, sizeof(mediaSockPollArray));

    while (m_RtpSession.isOpened() && !m_pleaseStop && !isDataTimerExpired())
    {
        while (m_gotData)
        {
            QnAbstractMediaDataPtr data = getNextDataInternal();
            if (data)
                return data;
            else
                m_gotData = false;
        }

        int nfds = 0;
        std::vector<int> fdToRtpIndex(m_tracks.size());
        fdToRtpIndex.clear();

        for (size_t i = 0; i < m_tracks.size(); ++i)
        {
            const TrackInfo& track = m_tracks[i];
            if (track.ioDevice)
            {
                if (!track.ioDevice->getMediaSocket())
                {
                    NX_ERROR(this, "%1: Invalid UDP socket handle", m_logName);
                    return QnAbstractMediaDataPtr();
                }
                mediaSockPollArray[nfds].fd = track.ioDevice->getMediaSocket()->handle();
                mediaSockPollArray[nfds++].events = POLLIN;
                fdToRtpIndex.push_back(i);
            }
        }
        mediaSockPollArray[nfds].fd = m_tcpSocket->handle();
        mediaSockPollArray[nfds++].events = POLLIN;

        const int rez = poll(mediaSockPollArray, nfds, MEDIA_DATA_READ_TIMEOUT_MS);
        if (rez < 1)
            continue;
        for (int i = 0; i < nfds; ++i)
        {
            if (!(mediaSockPollArray[i].revents & POLLIN))
                continue;
            if (mediaSockPollArray[i].fd == m_tcpSocket->handle())
            {
                m_RtpSession.readAndProcessTextData();
                continue;
            }

            const int rtpChannelNum = fdToRtpIndex[i];
            TrackInfo& track = m_tracks[rtpChannelNum];
            while (!m_gotData)
            {
                quint8* rtpBuffer = QnRtspClient::prepareDemuxedData(
                    m_demuxedData,
                    rtpChannelNum,
                    MAX_RTP_PACKET_SIZE); // todo: update here
                int bytesRead = track.ioDevice->read((char*) rtpBuffer, MAX_RTP_PACKET_SIZE);
                if (bytesRead < 1)
                {
                    auto errorCode = SystemError::getLastOSErrorCode();
                    if (errorCode != SystemError::timedOut && errorCode != SystemError::again
                        && errorCode != SystemError::wouldBlock
                        && errorCode != SystemError::noError)
                    {
                        NX_DEBUG(
                            this,
                            "%1: Failed to read from UDP socket, OS code %2: %3",
                            m_logName,
                            errorCode,
                            SystemError::toString(errorCode));
                    }
                    break;
                }
                NX_VERBOSE(this, "%1: %2 bytes read form UDP socket", m_logName, bytesRead);
                m_demuxedData[rtpChannelNum]->finishWriting(bytesRead);
                quint8* bufferBase = (quint8*) m_demuxedData[rtpChannelNum]->data();
                int offset = rtpBuffer - bufferBase;

                if (!processData(
                        track,
                        bufferBase,
                        offset,
                        bytesRead,
                        rtpChannelNum,
                        errorRetryCount))
                    return QnAbstractMediaDataPtr(0);
            }
        }
    }

    return QnAbstractMediaDataPtr();
}

nx::rtp::StreamParserPtr RtspStreamProvider::createParser(const QString& codecName)
{
    if (codecName.isEmpty())
        return nullptr;

    using namespace nx::rtp;
    if (codecName.compare(QString::fromUtf8(kFfmpegCodecName), Qt::CaseInsensitive) == 0)
        return std::make_unique<nx::rtp::QnNxRtpParser>();

    if (codecName.compare(QString::fromUtf8(kFfmpegMetadataCodecName), Qt::CaseInsensitive) == 0)
        return std::make_unique<nx::rtp::NxRtpMetadataParser>();

    if (codecName == QLatin1String("H264"))
        return std::make_unique<nx::rtp::H264Parser>();

    if (codecName == QLatin1String("H265"))
        return std::make_unique<nx::rtp::HevcParser>();

    if (codecName == QLatin1String("JPEG"))
        return std::make_unique<nx::rtp::MjpegParser>();

    if (codecName == QLatin1String("MP4V-ES"))
        return std::make_unique<nx::rtp::Mpeg4Parser>();

    if (codecName == QLatin1String("MPEG4-GENERIC"))
        return std::make_unique<nx::rtp::AacParser>();

    if (codecName == QLatin1String("MPA"))
        return std::make_unique<nx::rtp::Mpeg12AudioParser>();

    if (codecName == QLatin1String("PCMU"))
        return std::make_unique<nx::rtp::SimpleAudioParser>(AV_CODEC_ID_PCM_MULAW);

    if (codecName == QLatin1String("PCMA"))
        return std::make_unique<nx::rtp::SimpleAudioParser>(AV_CODEC_ID_PCM_ALAW);

    if (codecName.startsWith(QLatin1String("G726"))) // g726-24, g726-32 e.t.c
    {
        int bitRatePos = codecName.indexOf(QLatin1Char('-'));
        if (bitRatePos == -1)
            return nullptr;
        QString bitsPerSample = codecName.mid(bitRatePos + 1);
        auto parser = std::make_unique<nx::rtp::SimpleAudioParser>(AV_CODEC_ID_ADPCM_G726);
        parser->setBitsPerSample(bitsPerSample.toInt() / 8);
        return parser;
    }

    if (codecName == QLatin1String("L16"))
        return std::make_unique<nx::rtp::SimpleAudioParser>(AV_CODEC_ID_PCM_S16BE);

    for (const auto& factory: m_customParserFactories)
    {
        if (auto parser = factory->createParser(codecName))
            return parser;
    }

    return nullptr;
}

nx::rtp::StreamParserPtr RtspStreamProvider::createParser(int payloadType)
{
    for (const auto& factory: m_customParserFactories)
    {
        if (auto parser = factory->createParser(payloadType))
            return parser;
    }

    return nullptr;
}

nx::vms::api::RtpTransportType RtspStreamProvider::getRtpTransport() const
{
    // Server side setting for resource.
    if (m_rtpTransport != nx::vms::api::RtpTransportType::automatic)
        return m_rtpTransport;

    NX_MUTEX_LOCKER lock(&defaultTransportMutex());
    return s_defaultTransportToUse;
}

void RtspStreamProvider::setRtpTransport(nx::vms::api::RtpTransportType value)
{
    m_rtpTransport = value;
}

void RtspStreamProvider::addCustomTrackParserFactory(
    std::unique_ptr<nx::rtp::IRtpParserFactory> parserFactory)
{
    m_customParserFactories.emplace_back(std::move(parserFactory));
}

void RtspStreamProvider::setExtraPayloadTypes(std::vector<int> payloadTypes)
{
    m_extraPayloadTypes = std::move(payloadTypes);
}

std::chrono::microseconds RtspStreamProvider::translateTimestampFromCameraToVmsSystem(
    std::chrono::microseconds timestamp,
    int /*channelNumber*/)
{
    using namespace std::chrono;
    microseconds result;

    {
        NX_MUTEX_LOCKER lock(&m_tracksMutex);
        result = m_tracks.empty() || !m_tracks[0].timeHelper
            ? timestamp
            : m_tracks[0].timeHelper->replayAdjustmentFromHistory(timestamp);
    }

    if (nx::utils::ini().checkCameraTimestamps)
    {
        const microseconds vmsSystemTimestamp = QnSyncTime::currentTimePoint();
        const seconds threshold(nx::utils::ini().cameraTimestampThresholdS);
        NX_ASSERT(
            timestamp >= vmsSystemTimestamp - threshold,
            NX_FMT(
                "Camera ts %1us < VMS ts %2us by %3us (threshold %4s)",
                timestamp.count(),
                vmsSystemTimestamp.count(),
                (vmsSystemTimestamp - timestamp).count(),
                threshold.count()));
        NX_ASSERT(
            timestamp <= vmsSystemTimestamp + threshold,
            NX_FMT(
                "Camera ts %1us > VMS ts %2us by %3us (threshold %4s)",
                timestamp.count(),
                vmsSystemTimestamp.count(),
                (timestamp - vmsSystemTimestamp).count(),
                threshold.count()));
        NX_ASSERT(
            result >= vmsSystemTimestamp - threshold,
            NX_FMT(
                "Result ts %1us < VMS ts %2us by %3us (threshold %4s, camera ts %5us)",
                result.count(),
                vmsSystemTimestamp.count(),
                (vmsSystemTimestamp - result).count(),
                threshold.count(),
                timestamp.count()));
        NX_ASSERT(
            result <= vmsSystemTimestamp + threshold,
            NX_FMT(
                "Result ts %1us > VMS ts %2us by %3us (threshold %4s, camera ts %5us)",
                result.count(),
                vmsSystemTimestamp.count(),
                (result - vmsSystemTimestamp).count(),
                threshold.count(),
                timestamp.count()));
    }

    return result;
}

std::optional<nx::network::http::Credentials> RtspStreamProvider::credentials() const
{
    return m_credentials;
}

void RtspStreamProvider::setCredentials(const nx::network::http::Credentials& credentials)
{
    m_credentials = credentials;
}

void RtspStreamProvider::setUrl(const nx::Url& url)
{
    m_url = url;
}

CameraDiagnostics::Result RtspStreamProvider::openStream()
{
    m_logName = m_url.toString();
    m_pleaseStop = false;
    if (isStreamOpened())
        return CameraDiagnostics::NoErrorResult();
    m_gotSomeFrame = false;
    m_RtpSession.setTransport(getRtpTransport());

    m_registeredMulticastAddresses.clear();

    m_RtpSession.setCredentials(
        credentials().value_or(nx::network::http::Credentials()),
        m_preferredAuthScheme);

    {
        NX_MUTEX_LOCKER lock(&m_tracksMutex);
        m_tracks.clear();
    }
    m_trackIndices.clear();
    m_gotKeyDataInfo.clear();
    m_gotData = false;

    PlaybackRange playbackRange;
    {
        NX_MUTEX_LOCKER lock(&m_mutex);
        std::swap(playbackRange, m_playbackRange);
        if (playbackRange.isValid())
        {
            NX_VERBOSE(
                this,
                "%1: Passing playback range %2 to RtpSession::open",
                __func__,
                playbackRange);
        }
    }

    m_openStreamResult = m_RtpSession.open(m_url, playbackRange.startTimeUsec);
    if (m_openStreamResult.errorCode != CameraDiagnostics::ErrorCode::noError)
        return m_openStreamResult;

    if (!m_customParserFactories.empty())
    {
        const auto tracks = m_RtpSession.getTrackInfo();
        std::set<QString> additionalSupportedCodecs;
        for (const QnRtspClient::SDPTrackInfo& track: tracks)
        {
            const QString& codecName = track.sdpMedia.rtpmap.codecName;
            if (isCodecSupportedByCustomParserFactories(codecName))
                additionalSupportedCodecs.insert(codecName);
        }

        m_RtpSession.setAdditionalSupportedCodecs(std::move(additionalSupportedCodecs));
    }

    if (!m_RtpSession.isPlayNowMode() && !m_RtpSession.sendSetup())
    {
        NX_WARNING(this, "Can't open RTSP stream [%1], SETUP request has been failed", m_url);

        m_openStreamResult = CameraDiagnostics::RequestFailedResult(
            m_url.toString(),
            "Can't open RTSP stream: SETUP request has been failed");
        return m_openStreamResult;
    }

    if (const auto result = registerMulticastAddressesIfNeeded(); !result)
    {
        m_openStreamResult = result;
        return m_openStreamResult;
    }

    if (!m_RtpSession.sendPlay(
            playbackRange.startTimeUsec,
            playbackRange.endTimeUsec,
            m_RtpSession.getScale()))
    {
        NX_WARNING(this, "Can't open RTSP stream [%1], PLAY request has been failed", m_url);

        m_openStreamResult = CameraDiagnostics::RequestFailedResult(
            m_url.toString(),
            "Can't open RTSP stream: PLAY request has been failed");
        return m_openStreamResult;
    }

    m_numberOfVideoChannels = numberOfVideoChannels();
    at_numberOfVideoChannelsChanged();

    createTrackParsers();

    m_openStreamResult = CameraDiagnostics::NoErrorResult();
    NX_DEBUG(this, "Successfully open RTSP stream %1", m_url);

    return m_openStreamResult;
}

void RtspStreamProvider::registerPredefinedTrack(int rtpChannelNumber)
{
    const int channelNumber = rtpChannelNumber / 2;
    auto codecParser = channelNumber == nx::rtp::kMetadataChannelNumber
        ? createParser("FFMPEG-METADATA")
        : createParser("FFMPEG");
    const int payloadType = channelNumber == nx::rtp::kMetadataChannelNumber
        ? nx::rtp::kNxMetadataPayloadType
        : nx::rtp::kNxPayloadType;

    TrackInfo trackInfo;
    trackInfo.rtpParsers.emplace(
        payloadType,
        std::make_unique<nx::rtp::RtpParser>(payloadType, std::move(codecParser)));

    if (m_RtpSession.getActualTransport() == nx::vms::api::RtpTransportType::tcp)
        m_trackIndices[rtpChannelNumber] = channelNumber;

    NX_MUTEX_LOCKER lock(&m_tracksMutex);
    m_tracks.resize(std::max(m_tracks.size(), size_t(channelNumber + 1)));
    m_tracks[channelNumber] = std::move(trackInfo);
}

void RtspStreamProvider::createTrackParsers()
{
    using namespace nx::streaming;
    auto& trackInfo = m_RtpSession.getTrackInfo();
    int logicalVideoNum = 0;

    for (auto& track: trackInfo)
    {
        if (!track.setupSuccess || !track.ioDevice || !isFormatSupported(track.sdpMedia))
            continue;

        TrackInfo trackInfo;
        auto codecParser = createParser(track.sdpMedia.rtpmap.codecName.toUpper());
        if (!codecParser)
        {
            NX_WARNING(
                this,
                "Failed to create track parser for codec: [%1]",
                track.sdpMedia.rtpmap.codecName);
            continue;
        }
        codecParser->setLogId(m_logName.toStdString());
        codecParser->setSdpInfo(track.sdpMedia);
        auto audioParser = dynamic_cast<nx::rtp::AudioStreamParser*>(codecParser.get());
        if (audioParser)
        {
            auto codecParamters = audioParser->getCodecParameters();
            if (codecParamters)
                m_audioLayout.reset(new AudioLayout(codecParamters));
        }

        trackInfo.rtpParsers.emplace(
            track.sdpMedia.payloadType,
            std::make_unique<nx::rtp::RtpParser>(
                track.sdpMedia.payloadType,
                std::move(codecParser)));

        for (int type: m_extraPayloadTypes)
        {
            if (auto parser = createParser(type))
            {
                parser->setSdpInfo(track.sdpMedia);
                trackInfo.rtpParsers.emplace(
                    type,
                    std::make_unique<nx::rtp::RtpParser>(type, std::move(parser)));
            }
        }

        trackInfo.ioDevice = track.ioDevice.get();
        trackInfo.rtcpChannelNumber = track.interleaved.second;
        if (!m_forceCameraTime)
        {
            trackInfo.timeHelper = std::make_unique<nx::streaming::rtp::CameraTimeHelper>(
                track.sdpMedia.mediaType,
                timeHelperKey(),
                m_timeOffset);
        }

        trackInfo.timeHelper->setTimePolicy(getTimePolicy());

        if (track.sdpMedia.mediaType == nx::rtp::Sdp::MediaType::Video)
            trackInfo.logicalChannelNum = logicalVideoNum++;
        else
            trackInfo.logicalChannelNum = m_numberOfVideoChannels;

        if (m_RtpSession.getActualTransport() == nx::vms::api::RtpTransportType::tcp)
            m_trackIndices[track.interleaved.first] = m_tracks.size();

        NX_MUTEX_LOCKER lock(&m_tracksMutex);
        m_tracks.emplace_back(std::move(trackInfo));
    }
}

nx::Mutex& RtspStreamProvider::defaultTransportMutex()
{
    static nx::Mutex s_defaultTransportMutex;
    return s_defaultTransportMutex;
}

void RtspStreamProvider::closeStream()
{
    if (m_RtpSession.isOpened())
        m_RtpSession.sendTeardown();
    m_RtpSession.stop();
    for (unsigned int i = 0; i < m_demuxedData.size(); ++i)
    {
        if (m_demuxedData[i])
            m_demuxedData[i]->clear();
    }

    m_registeredMulticastAddresses.clear();
}

bool RtspStreamProvider::isStreamOpened() const
{
    return m_RtpSession.isOpened();
}

CameraDiagnostics::Result RtspStreamProvider::lastOpenStreamResult() const
{
    return m_openStreamResult;
}

AudioLayoutConstPtr RtspStreamProvider::getAudioLayout() const
{
    return m_audioLayout;
}

void RtspStreamProvider::pleaseStop()
{
    m_pleaseStop = true;
    m_RtpSession.shutdown();
}

void RtspStreamProvider::setDefaultTransport(nx::vms::api::RtpTransportType rtpTransport)
{
    NX_INFO(typeid(RtspStreamProvider), "Set default transport: %1", rtpTransport);

    NX_MUTEX_LOCKER lock(&defaultTransportMutex());
    s_defaultTransportToUse = rtpTransport;
}

void RtspStreamProvider::setPreferredAuthScheme(
    const nx::network::http::header::AuthScheme::Value scheme)
{
    m_preferredAuthScheme = scheme;
}

QnConstResourceVideoLayoutPtr RtspStreamProvider::getVideoLayout() const
{
    NX_MUTEX_LOCKER lock(&m_layoutMutex);
    return m_customVideoLayout;
}

void RtspStreamProvider::setUserAgent(const QString& value)
{
    m_RtpSession.setUserAgent(value);
}

nx::Url RtspStreamProvider::getCurrentStreamUrl() const
{
    return m_url;
}

void RtspStreamProvider::setPlaybackRange(int64_t startTimeUsec, int64_t endTimeUsec)
{
    return setPlaybackRange(PlaybackRange(startTimeUsec, endTimeUsec));
}

void RtspStreamProvider::setPlaybackRange(const PlaybackRange& playbackRange)
{
    NX_MUTEX_LOCKER lock(&m_mutex);
    m_playbackRange = playbackRange;
    NX_VERBOSE(this, "%1: Playback range set to %2", __func__, m_playbackRange);
}

void RtspStreamProvider::setDateTimeFormat(const QnRtspClient::DateTimeFormat& format)
{
    m_RtpSession.setDateTimeFormat(format);
}

void RtspStreamProvider::addRequestHeader(
    const QString& requestName,
    const nx::network::http::HttpHeader& header)
{
    m_RtpSession.addRequestHeader(requestName, header);
}

QnRtspClient& RtspStreamProvider::rtspClient()
{
    return m_RtpSession;
}

bool RtspStreamProvider::isFormatSupported(const nx::rtp::Sdp::Media media) const
{
    return media.mediaType == nx::rtp::Sdp::MediaType::Audio
        || media.mediaType == nx::rtp::Sdp::MediaType::Video
        || isCodecSupportedByCustomParserFactories(media.rtpmap.codecName);
}

void RtspStreamProvider::setOnSocketReadTimeoutCallback(
    std::chrono::milliseconds timeout,
    OnSocketReadTimeoutCallback callback)
{
    m_callbackTimeout = timeout;
    m_onSocketReadTimeoutCallback = std::move(callback);
}

void RtspStreamProvider::setRtpFrameTimeoutMs(int value)
{
    m_rtpFrameTimeoutMs = value;
    m_RtpSession.setTCPTimeout(std::chrono::milliseconds(m_rtpFrameTimeoutMs));
}

void RtspStreamProvider::setConnectionTimeout(std::chrono::milliseconds timeout)
{
    m_RtpSession.setConnectionTimeout(timeout);
}

std::chrono::milliseconds RtspStreamProvider::connectionTimeout() const
{
    return m_RtpSession.connectionTimeout();
}

nx::streaming::rtp::TimePolicy RtspStreamProvider::getTimePolicy() const
{
    return nx::streaming::rtp::TimePolicy::useCameraTimeIfCorrect;
}

CameraDiagnostics::Result RtspStreamProvider::registerMulticastAddressesIfNeeded()
{
    // Multicast is not supported for the base class. It is needed to refactor MulticastRegistry
    // class if it is will be required in the future.
    return CameraDiagnostics::NoErrorResult();
}

int RtspStreamProvider::numberOfVideoChannels() const
{
    return m_RtpSession.getTrackCount(nx::rtp::Sdp::MediaType::Video);
}

std::string RtspStreamProvider::timeHelperKey() const
{
    return m_url.toStdString();
}

} // namespace nx::streaming
