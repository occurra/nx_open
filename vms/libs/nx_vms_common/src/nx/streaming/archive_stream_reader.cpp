// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "archive_stream_reader.h"

#include <stdint.h>

#include <QtCore/QThread>

#include <media/filters/abstract_media_data_filter.h>
#include <nx/media/video_data_packet.h>
#include <nx/streaming/abstract_data_consumer.h>
#include <nx/utils/log/log.h>
#include <recording/time_period.h>
#include <recording/time_period_list.h>
#include <utils/common/synctime.h>
#include <utils/common/util.h>
#include <utils/media/frame_type_extractor.h>

using namespace nx::vms::api;

// used in reverse mode.
// seek by 1.5secs. It is prevents too fast seeks for short GOP, also some codecs has bagged seek
// function. Large step prevent seek forward instead seek backward
// static const int MAX_KEY_FIND_INTERVAL = 10'000'000;

// static const int FFMPEG_PROBE_BUFFER_SIZE = 1024 * 512;
static const qint64 LIVE_SEEK_OFFSET = 10'000'000ll;

constexpr auto kSingleShowWaitTimeoutMSec = 100;

QnArchiveStreamReader::QnArchiveStreamReader(const nx::Uuid& deviceId, const QString& url):
    QnAbstractMediaStreamDataProvider(url),
    m_deviceId(deviceId)
{
}

/*
QnArchiveStreamReader::onStatusChanged(nx::vms::api::ResourceStatus oldStatus,
nx::vms::api::ResourceStatus newStatus)
{
    if (newStatus == Qn::Offline)
        m_delegate->close();
}
*/

QnArchiveStreamReader::~QnArchiveStreamReader()
{
    stop();

    delete m_frameTypeExtractor;
    m_frameTypeExtractor = 0;
}

bool QnArchiveStreamReader::open()
{
    return m_delegate && m_delegate->open(m_url);
}

void QnArchiveStreamReader::nextFrame()
{
    emit nextFrameOccurred();
    NX_MUTEX_LOCKER lock(&m_jumpMtx);
    m_singleQuantProcessed = false;
    m_singleShowWaitCond.wakeAll();
}

void QnArchiveStreamReader::needMoreData()
{
    NX_MUTEX_LOCKER lock(&m_jumpMtx);
    m_singleQuantProcessed = false;
    m_singleShowWaitCond.wakeAll();
}

void QnArchiveStreamReader::previousFrame(qint64 usec)
{
    --usec;
    emit prevFrameOccurred();
    jumpToPreviousFrame(usec);
}

void QnArchiveStreamReader::resumeMedia()
{
    resetRealtimeDelay();
    if (m_singleShot)
    {
        emit streamAboutToBeResumed();
        m_delegate->setSingleshotMode(false);
        m_singleShot = false;
        // resumeDataProcessors();
        NX_MUTEX_LOCKER lock(&m_jumpMtx);
        m_singleShowWaitCond.wakeAll();

        lock.unlock();
        emit streamResumed();
    }
}

void QnArchiveStreamReader::pauseMedia()
{
    if (!m_singleShot)
    {
        emit streamAboutToBePaused();
        NX_MUTEX_LOCKER lock(&m_jumpMtx);
        m_singleShot = true;
        m_singleQuantProcessed = true;
        m_delegate->setSingleshotMode(true);

        lock.unlock();
        emit streamPaused();
    }
}

bool QnArchiveStreamReader::isMediaPaused() const
{
    return m_singleShot;
}

void QnArchiveStreamReader::setCurrentTime(qint64 value)
{
    NX_MUTEX_LOCKER mutex(&m_jumpMtx);
    m_currentTime = value;
}

std::chrono::microseconds QnArchiveStreamReader::currentTime() const
{
    NX_MUTEX_LOCKER mutex(&m_jumpMtx);
    return std::chrono::microseconds(m_skipFramesToTime ? m_skipFramesToTime : m_currentTime);
}

void QnArchiveStreamReader::reopen()
{
    auto currentTime = this->currentTime().count();
    auto isMediaPaused = this->isMediaPaused();
    auto speed = this->getSpeed();

    getArchiveDelegate()->reopen();

    setCurrentTime(currentTime);
    setSpeed(speed);
    if (isMediaPaused)
        pauseMedia();
}

QnConstResourceVideoLayoutPtr QnArchiveStreamReader::getDPVideoLayout() const
{
    if (!(m_delegate->getFlags() & QnAbstractArchiveDelegate::Flag_CanOfflineLayout))
        m_delegate->open(m_url);
    return m_delegate->getVideoLayout();
}

bool QnArchiveStreamReader::hasVideo() const
{
    if (!m_hasVideo.has_value())
    {
        if (!(m_delegate->getFlags() & QnAbstractArchiveDelegate::Flag_CanOfflineHasVideo))
        {
            m_delegate->open(m_url);
        }
        m_hasVideo = m_delegate->hasVideo();
    }
    return *m_hasVideo;
}

AudioLayoutConstPtr QnArchiveStreamReader::getAudioLayout() const
{
    if (!m_audioLayout.has_value())
    {
        if (!(m_delegate->getFlags() & QnAbstractArchiveDelegate::Flag_CanOfflineLayout))
            m_delegate->open(m_url);
        m_audioLayout = m_delegate->getAudioLayout();
    }
    return *m_audioLayout;
}

bool QnArchiveStreamReader::init()
{
    setCurrentTime(0);

    m_jumpMtx.lock();
    qint64 requiredJumpTime = m_requiredJumpTime;
    bool usePreciseSeek = m_lastUsePreciseSeek;
    MediaQuality quality = m_quality;
    QSize resolution = m_customResolution;
    auto streamDataFilter = m_streamDataFilter;
    qint64 jumpTime = qint64(AV_NOPTS_VALUE);
    bool imSeek = m_delegate->getFlags() & QnAbstractArchiveDelegate::Flag_CanSeekImmediatly;
    const double speed = m_speed;
    if (imSeek)
    {
        if (requiredJumpTime != qint64(AV_NOPTS_VALUE))
        {
            jumpTime = requiredJumpTime;
            m_skipFramesToTime = m_tmpSkipFramesToTime;
            m_tmpSkipFramesToTime = 0;
        }
        else if (speed < 0)
        {
            jumpTime = QnSyncTime::currentUSecsSinceEpoch();
        }
    }

    m_jumpMtx.unlock();

    auto canProcessSpeedBeforeOpen = [&](double speed)
    {
        // Ignore zero speed if item is opened on pause.
        // It needs at least one frame to be loaded.
        if (qFuzzyIsNull(speed))
            return false;
        bool imSeek =
            m_delegate->getFlags().testFlag(QnAbstractArchiveDelegate::Flag_CanSeekImmediatly);
        if (!imSeek)
            return false;
        bool negativeSpeedSupported = m_delegate->getFlags().testFlag(
            QnAbstractArchiveDelegate::Flag_CanProcessNegativeSpeed);
        if (speed < 0 && !negativeSpeedSupported)
            return false;
        return true;
    };

    m_delegate->setQuality(quality, true, resolution);
    bool isSpeedCommandProcessed = false;
    // It is optimization: open and jump at same time
    if (jumpTime != qint64(AV_NOPTS_VALUE))
    {
        if (canProcessSpeedBeforeOpen(speed))
        {
            m_delegate->setSpeed(jumpTime, speed);
            isSpeedCommandProcessed = true;
        }
        else
        {
            m_delegate->seek(jumpTime, true);
        }
    }

    m_prevStreamDataFilter = streamDataFilter;
    m_delegate->setStreamDataFilter(streamDataFilter);
    bool opened = m_delegate->open(m_url);

    if (jumpTime != qint64(AV_NOPTS_VALUE))
        emitJumpOccurred(jumpTime, usePreciseSeek, m_delegate->getSequence());

    if (!opened)
        return false;

    m_delegate->setAudioChannel(m_selectedAudioChannel);

    m_jumpMtx.lock();
    m_oldQuality = quality;
    m_oldQualityFastSwitch = true;
    m_oldResolution = resolution;
    if (isSpeedCommandProcessed)
        m_prevSpeed = speed;
    m_jumpMtx.unlock();

    // Alloc common resources

    return true;
}

bool QnArchiveStreamReader::offlineRangeSupported() const
{
    return m_delegate->getFlags() & QnAbstractArchiveDelegate::Flag_CanOfflineRange;
}

qint64 QnArchiveStreamReader::determineDisplayTime(bool reverseMode)
{
    auto dataProcessors = m_dataprocessors.lock();
    for (int i = 0; i < dataProcessors->size(); ++i)
    {
        if (auto dp = dynamic_cast<QnAbstractDataConsumer*>(dataProcessors->at(i)))
        {
            if (dp->isRealTimeSource())
                return DATETIME_NOW;
        }
    }

    if (m_lastSeekPosition != AV_NOPTS_VALUE)
        return m_lastSeekPosition;

    if (reverseMode)
        return endTime();

    return startTime();
}

QnAbstractMediaDataPtr QnArchiveStreamReader::createEmptyPacket(bool isReverseMode)
{
    QnAbstractMediaDataPtr rez(new QnEmptyMediaData());
    rez->timestamp = isReverseMode ? 0 : DATETIME_NOW;
    if (m_BOF)
        rez->flags |= QnAbstractMediaData::MediaFlags_BOF;
    if (m_eof)
        rez->flags |= QnAbstractMediaData::MediaFlags_AfterEOF;
    if (isReverseMode)
        rez->flags |= QnAbstractMediaData::MediaFlags_Reverse;
    if (m_dataMarker)
        rez->opaque = m_dataMarker;
    else
        rez->opaque = m_delegate->getSequence();
    QThread::msleep(50);
    return rez;
}

void QnArchiveStreamReader::startPaused(qint64 startTime)
{
    m_singleShot = true;
    m_singleQuantProcessed = false;
    m_requiredJumpTime = m_lastSeekPosition = m_tmpSkipFramesToTime = startTime;
    start();
}

bool QnArchiveStreamReader::isCompatiblePacketForMask(
    const QnAbstractMediaDataPtr& mediaData) const
{
    if (hasVideo())
    {
        if (mediaData->dataType != QnAbstractMediaData::VIDEO)
            return false;
    }
    else
    {
        if (mediaData->dataType != QnAbstractMediaData::AUDIO)
            return false;
    }
    return !mediaData->isLive();
}

QnAbstractMediaDataPtr QnArchiveStreamReader::getNextData()
{
    NX_VERBOSE(this, "Next data requested from %1", m_url);

    if (m_stopCond)
    {
        NX_MUTEX_LOCKER lock(&m_stopMutex);
        m_delegate->close();
        while (m_stopCond && !needToStop())
            m_stopWaitCond.wait(&m_stopMutex);
        if (needToStop())
        {
            NX_VERBOSE(this, "Need to stop(m_stopCond), return null packet");
            return QnAbstractMediaDataPtr();
        }
        m_delegate->seek(m_latPacketTime, true);
    }

wait_label:
    // =================
    {
        NX_MUTEX_LOCKER mutex(&m_jumpMtx);
        while (m_singleShot && m_skipFramesToTime == 0 && m_singleQuantProcessed
               && m_requiredJumpTime == qint64(AV_NOPTS_VALUE) && !needToStop() && !isPaused())
        {
            m_singleShowWaitCond.wait(&m_jumpMtx, kSingleShowWaitTimeoutMSec);
        }
        // QnLongRunnable::pause();
    }

    bool singleShotMode = m_singleShot;

begin_label:
    if (needToStop())
    {
        NX_VERBOSE(this, "Need to stop, return null packet");
        return QnAbstractMediaDataPtr();
    }

    if (m_firstTime)
    {
        // this is here instead if constructor to unload ui thread
        m_BOF = true;
        if (init())
        {
            m_firstTime = false;
        }
        else
        {
            m_firstTime = false; //< Do not try to reopen if it can't be opened.
            // If media data can't be opened report 'no data'
            NX_DEBUG(this, "Failed to init, return empty packet");
            return createEmptyPacket(isReverseMode());
        }
    }

    const auto streamDataFilter = m_streamDataFilter;
    if (streamDataFilter != m_prevStreamDataFilter)
    {
        m_delegate->setStreamDataFilter(streamDataFilter);
        m_prevStreamDataFilter = streamDataFilter;
    }

    int channelCount =
        m_delegate->getVideoLayout() ? m_delegate->getVideoLayout()->channelCount() : 1;

    m_jumpMtx.lock();
    double speed = m_speed;
    const bool reverseMode = speed < 0;
    const bool prevReverseMode = m_prevSpeed < 0;

    qint64 jumpTime = m_requiredJumpTime;
    bool usePreciseSeek = m_lastUsePreciseSeek;
    MediaQuality quality = m_quality;
    bool qualityFastSwitch = m_qualityFastSwitch;
    QSize resolution = m_customResolution;
    qint64 tmpSkipFramesToTime = m_tmpSkipFramesToTime;
    m_tmpSkipFramesToTime = 0;
    bool skipToFrameBeforeTarget = m_tmpSkipToFrameBeforeTarget;
    m_tmpSkipToFrameBeforeTarget = false;
    bool exactJumpToSpecifiedFrame = m_exactJumpToSpecifiedFrame;
    qint64 currentTimeHint = m_currentTimeHint;
    m_currentTimeHint = AV_NOPTS_VALUE;

    bool needChangeQuality = m_oldQuality != quality || qualityFastSwitch > m_oldQualityFastSwitch
        || resolution != m_oldResolution;
    if (needChangeQuality)
    {
        NX_VERBOSE(
            this,
            "Change quality from %1 to %2, fast switch from %3 to %4, resolution from %5 to %6",
            m_oldQuality,
            quality,
            m_oldQualityFastSwitch,
            qualityFastSwitch,
            m_oldResolution,
            resolution);
        m_oldQuality = quality;
        m_oldQualityFastSwitch = qualityFastSwitch;
        m_oldResolution = resolution;
    }

    m_dataMarker = m_newDataMarker;

    m_jumpMtx.unlock();

    // change quality checking
    if (needChangeQuality)
    {
        // !m_delegate->isRealTimeSource()
        bool needSeek = m_delegate->setQuality(quality, qualityFastSwitch, resolution);
        if (needSeek && jumpTime == qint64(AV_NOPTS_VALUE) && reverseMode == prevReverseMode)
        {
            qint64 displayTime = determineDisplayTime(reverseMode);
            if (displayTime != qint64(AV_NOPTS_VALUE))
            {
                NX_VERBOSE(
                    this,
                    "Jump to %1 to change quality",
                    nx::utils::timestampToDebugString(displayTime));
                beforeJumpInternal(displayTime);
                if (!exactJumpToSpecifiedFrame && channelCount > 1)
                    setNeedKeyData();
                m_outOfPlaybackMask = false;
                internalJumpTo(displayTime);
                if (displayTime != DATETIME_NOW)
                    m_skipFramesToTime = displayTime;

                emitJumpOccurred(displayTime, usePreciseSeek, m_delegate->getSequence());
                m_BOF = true;
            }
        }
    }

    // jump command
    if (jumpTime != qint64(AV_NOPTS_VALUE)
        && reverseMode == prevReverseMode) // if reverse mode is changing, ignore seek, because of
                                           // reverseMode generate seek operation
    {
        m_outOfPlaybackMask = false;
        m_skipFramesToTime = tmpSkipFramesToTime;
        m_skipToFrameBeforeTarget = skipToFrameBeforeTarget;
        m_bufferedVideoData.reset(); //< Drop any stale one-frame look-ahead buffer.
        m_ignoreSkippingFrame = exactJumpToSpecifiedFrame;
        if (!exactJumpToSpecifiedFrame && channelCount > 1)
            setNeedKeyData();
        internalJumpTo(jumpTime);
        emitJumpOccurred(jumpTime, usePreciseSeek, m_delegate->getSequence());
        m_BOF = true;
    }

    // reverse mode changing
    bool delegateForNegativeSpeed =
        m_delegate->getFlags() & QnAbstractArchiveDelegate::Flag_CanProcessNegativeSpeed;

    auto displayTimeToJump = [&]()
    {
        qint64 displayTime = currentTimeHint;
        if (currentTimeHint == DATETIME_NOW)
        {
            if (reverseMode)
                displayTime = endTime() - BACKWARD_SEEK_STEP;
        }
        else if (currentTimeHint == qint64(AV_NOPTS_VALUE))
        {
            if (jumpTime != qint64(AV_NOPTS_VALUE))
                displayTime = jumpTime;
            else
                displayTime = determineDisplayTime(reverseMode);
        }
        return displayTime;
    };

    if (reverseMode != prevReverseMode)
    {
        if (jumpTime != qint64(AV_NOPTS_VALUE))
            currentTimeHint = jumpTime;
        m_outOfPlaybackMask = false;
        m_bofReached = false;
        qint64 displayTime = displayTimeToJump();

        m_delegate->setSpeed(displayTime, speed);

        if (!delegateForNegativeSpeed)
        {
            if (!exactJumpToSpecifiedFrame && channelCount > 1)
                setNeedKeyData();
            internalJumpTo(displayTime);
            if (reverseMode)
            {
                if (displayTime != DATETIME_NOW)
                    m_topIFrameTime = displayTime;
            }
            else
                m_skipFramesToTime = displayTime;
        }
        else
        {
            if (!reverseMode && displayTime != DATETIME_NOW
                && displayTime != qint64(AV_NOPTS_VALUE))
                m_skipFramesToTime = displayTime;
        }

        m_lastGopSeekTime = -1;
        m_BOF = true;
        m_afterBOFCounter = 0;
        if (jumpTime != qint64(AV_NOPTS_VALUE))
            emitJumpOccurred(displayTime, usePreciseSeek, m_delegate->getSequence());
    }
    else if (speed != m_prevSpeed)
    {
        m_delegate->setSpeed(displayTimeToJump(), speed);
    }

    m_prevSpeed = speed;

    if (m_outOfPlaybackMask)
    {
        if (m_endOfPlaybackHandler)
            m_endOfPlaybackHandler();

        auto result = createEmptyPacket(reverseMode); // EOF reached
        result->flags |= QnAbstractMediaData::MediaFlags_AfterEOF;
        NX_DEBUG(this, "EOF, return empty packet");
        return result;
    }

    if (m_afterMotionData)
    {
        QnAbstractMediaDataPtr result;
        result.swap(m_afterMotionData);
        return result;
    }

    // The crossing frame held back by a previousFrame() step (see the skip loop). A
    // jump would have dropped it above; with no jump pending this nextFrame() reveals
    // it now, and the shot ends so the reader blocks again on the next call.
    if (m_bufferedVideoData)
    {
        QnAbstractMediaDataPtr result;
        result.swap(m_bufferedVideoData);
        result->flags &= ~QnAbstractMediaData::MediaFlags_PreRoll;
        if (m_singleShot)
        {
            NX_MUTEX_LOCKER lock(&m_jumpMtx);
            m_singleQuantProcessed = true;
        }
        return result;
    }

    if (m_delegate->startTime() == qint64(AV_NOPTS_VALUE))
    {
        auto result = createEmptyPacket(reverseMode); //< No data at archive
        result->flags |= QnAbstractMediaData::MediaFlags_AfterEOF;
        NX_DEBUG(this, "No data in archive, return empty packet");
        return result;
    }
    QnCompressedVideoDataPtr videoData;

    if (m_skipFramesToTime != 0)
        m_lastGopSeekTime = -1; // after user seek

    m_currentData = getNextPacket();

    if (m_currentData == 0)
    {
        NX_DEBUG(this, "Null packet from delegate, return null packet");
        return m_currentData;
    }

    if (m_currentData->flags & QnAbstractMediaData::MediaFlags_Skip)
        goto begin_label;

    videoData = std::dynamic_pointer_cast<QnCompressedVideoData>(m_currentData);

    if (m_currentData->timestamp != qint64(AV_NOPTS_VALUE))
    {
        setCurrentTime(m_currentData->timestamp);
    }

    // If of archive is reached for reverse mode it need to continue in two cases:
    //  1. it is right (but not left) edge, it need to generate next seek operation
    //  2. Cycle mode flag is set. It need to jump to the end of archive after begin of archive is
    //  reached.
    const bool needContinueAfterEof = m_eof && (!m_bofReached || m_cycleMode);
    if (videoData || needContinueAfterEof)
    {
        if (reverseMode && !delegateForNegativeSpeed)
        {
            // I have found example where AV_PKT_FLAG_KEY detected very bad.
            // Same frame sometimes Key sometimes not. It is VC1 codec.
            // Manual detection for it stream better, but has artifacts too. I thinks some data
            // lost in stream after jump (after sequence header always P-frame, not I-Frame. But I
            // returns I, because no I frames at all in other case)

            bool isKeyFrame = false;
            if (videoData)
            {
                isKeyFrame = m_currentData->isKeyFrame();
                if (videoData->context)
                {
                    if (m_frameTypeExtractor == 0
                        || videoData->context.get() != m_frameTypeExtractor->getContext().get())
                    {
                        delete m_frameTypeExtractor;
                        m_frameTypeExtractor = new FrameTypeExtractor(videoData->context);
                    }
                }

                if (m_frameTypeExtractor)
                {
                    const auto frameType = m_frameTypeExtractor->getFrameType(
                        (const quint8*) videoData->data(),
                        static_cast<int>(videoData->dataSize()));

                    if (frameType != FrameTypeExtractor::UnknownFrameType)
                        isKeyFrame = frameType == FrameTypeExtractor::I_Frame;
                }
            }

            if (m_eof
                || (m_currentTime == 0 && m_bottomIFrameTime > 0
                    && m_topIFrameTime >= m_bottomIFrameTime))
            {
                // seek from EOF to BOF occurred
                // NX_ASSERT(m_topIFrameTime != DATETIME_NOW);
                setCurrentTime(m_topIFrameTime);
                m_eof = false;
            }

            // Limitation for duration of the first GOP after reverse mode activation
            if (m_afterBOFCounter != -1)
            {
                if (m_afterBOFCounter == 0 && m_currentTime == std::numeric_limits<qint64>::max())
                {
                    // no any packet yet read from archive and eof reached. So, current time still
                    // unknown
                    QThread::msleep(10);
                    internalJumpTo(QnSyncTime::currentUSecsSinceEpoch() - BACKWARD_SEEK_STEP);
                    m_afterBOFCounter = 0;
                    goto begin_label;
                }

                m_afterBOFCounter++;
                if (m_afterBOFCounter >= MAX_FIRST_GOP_FRAMES)
                {
                    m_topIFrameTime = m_currentTime;
                    m_afterBOFCounter = -1;
                }
            }

            // multisensor cameras support
            if (videoData)
            {
                int ch = videoData->channelNumber;
                if (ch > 0 && !m_rewSecondaryStarted[ch])
                {
                    if (isKeyFrame)
                    {
                        videoData->flags |= QnAbstractMediaData::MediaFlags_ReverseBlockStart;
                        m_rewSecondaryStarted[ch] = true;
                    }
                    else
                        goto begin_label; // skip
                }
            }

            if (isKeyFrame || m_currentTime >= m_topIFrameTime)
            {
                if (videoData && m_bottomIFrameTime == -1 && m_currentTime < m_topIFrameTime)
                {
                    m_bottomIFrameTime = m_currentTime;
                    videoData->flags |= QnAbstractMediaData::MediaFlags_ReverseBlockStart;
                    memset(&m_rewSecondaryStarted, 0, sizeof(m_rewSecondaryStarted));
                }
                if (m_currentTime >= m_topIFrameTime)
                {
                    qint64 seekTime;
                    if (m_bofReached)
                    {
                        if (m_cycleMode)
                        {
                            if (m_delegate->endTime() != DATETIME_NOW)
                            {
                                m_topIFrameTime = m_delegate->endTime();
                                m_bottomIFrameTime = seekTime =
                                    m_topIFrameTime - BACKWARD_SEEK_STEP;
                            }
                            else
                            {
                                m_topIFrameTime = QnSyncTime::currentUSecsSinceEpoch();
                                seekTime = m_topIFrameTime - LIVE_SEEK_OFFSET;
                            }
                        }
                        else
                        {
                            m_eof = true;
                            NX_DEBUG(this, "EOF reached, return empty packet");
                            return createEmptyPacket(reverseMode);
                        }
                    }
                    else
                    {
                        // sometime av_file_ssek doesn't seek to key frame (seek direct to
                        // specified position) So, no KEY frame may be found after seek. At this
                        // case (m_bottomIFrameTime == -1) we increase seek interval
                        qint64 ct = m_currentTime != DATETIME_NOW
                            ? m_currentTime - BACKWARD_SEEK_STEP
                            : m_currentTime;
                        seekTime = m_bottomIFrameTime != -1
                            ? m_bottomIFrameTime
                            : (m_lastGopSeekTime != -1 ? m_lastGopSeekTime : ct);
                        if (seekTime != DATETIME_NOW)
                            seekTime =
                                qMax(m_delegate->startTime(), seekTime - BACKWARD_SEEK_STEP);
                        else
                            seekTime = QnSyncTime::currentUSecsSinceEpoch() - BACKWARD_SEEK_STEP;
                    }

                    if (m_currentTime != seekTime)
                    {
                        m_currentData.reset();
                        qint64 tmpVal =
                            m_bottomIFrameTime != -1 ? m_bottomIFrameTime : m_topIFrameTime;
                        internalJumpTo(seekTime);
                        m_bofReached =
                            (seekTime == m_delegate->startTime()) || m_topIFrameTime > seekTime;
                        m_lastGopSeekTime = m_topIFrameTime; // seekTime;
                        // NX_ASSERT(m_lastGopSeekTime < DATETIME_NOW/2000ll);
                        m_topIFrameTime = tmpVal;
                        // return getNextData();
                        goto begin_label;
                    }
                    else
                    {
                        m_bottomIFrameTime = m_currentTime;
                        m_topIFrameTime = m_currentTime + BACKWARD_SEEK_STEP;
                    }
                }
            }
            else if (m_bottomIFrameTime == -1)
            {
                // invalid seek. must be key frame
                m_currentData = QnAbstractMediaDataPtr();
                // return getNextData();
                goto begin_label;
            }
        } // negative speed
    } // videoData || eof

    if (m_currentData->isLive())
        m_skipFramesToTime = 0;

    if (videoData) // in case of video packet
    {
        if (m_skipFramesToTime)
        {
            // Precise seek. Flag every frame decoded from the seek key frame as
            // pre-roll, through the first one whose OWN timestamp reaches the
            // requested time - that frame ends the skip. Deciding on the current
            // frame's timestamp (not peeking the next frame's, as this did
            // before) makes that last pre-roll frame the first one at or after
            // the request instead of the last one before it, so a precise seek
            // lands AT the requested moment - the previous "last frame before"
            // left the WebRTC player one frame short at low frame rates. The
            // target frame stays flagged pre-roll too (aligned with reverse
            // playback); the player reveals it by its UTC, so the flag need not
            // be cleared here.
            const bool reachedTarget = !reverseMode
                ? m_currentData->timestamp >= m_skipFramesToTime
                : m_currentData->timestamp <= m_skipFramesToTime;
            videoData->flags |= QnAbstractMediaData::MediaFlags_PreRoll;

            if (reachedTarget)
            {
                m_skipFramesToTime = 0;
                // In single-shot (paused) mode the target is the last frame of the
                // seek. It stays flagged pre-roll (above), so the generic "first
                // non-pre-roll frame ends the single shot" gate below would pull one
                // more frame past the requested position - a frame that also escapes
                // the pre-roll/BOF playout-delay tagging on the WebRTC path. End the
                // single shot right here so playback stops exactly at the target and
                // every delivered frame carries the pre-roll flag.
                if (singleShotMode)
                    m_singleQuantProcessed = true;

                if (m_skipToFrameBeforeTarget)
                {
                    // Step backward (previousFrame): the frame just before the target
                    // was already delivered on the previous single-shot call, since
                    // skip frames are handed out one per call. Do not deliver this
                    // crossing frame now - stash it so a following nextFrame() reveals
                    // it (see the m_bufferedVideoData drain above) - and re-enter the
                    // wait loop, leaving the frame before the target as the last one
                    // delivered.
                    m_skipToFrameBeforeTarget = false;
                    m_bufferedVideoData = m_currentData;
                    goto wait_label;
                }
            }
        }
    }
    else if (m_currentData->dataType == QnAbstractMediaData::AUDIO)
    {
        if (m_skipFramesToTime && m_currentData->timestamp < m_skipFramesToTime)
            goto begin_label;
    }
    else if (m_currentData->dataType == QnAbstractMediaData::EMPTY_DATA)
    {
        m_skipFramesToTime = 0;
    }

    if (videoData && (videoData->flags & QnAbstractMediaData::MediaFlags_PreRoll)
        && m_ignoreSkippingFrame)
        goto begin_label;

    if (reverseMode && !delegateForNegativeSpeed)
        m_currentData->flags |= QnAbstractMediaData::MediaFlags_Reverse;

    if (videoData && singleShotMode
        && !(videoData->flags.testFlag(QnAbstractMediaData::MediaFlags_PreRoll)))
    {
        m_singleQuantProcessed = true;
        // m_currentData->flags |= QnAbstractMediaData::MediaFlags_SingleShot;
    }
    if (m_currentData && m_dataMarker)
        m_currentData->opaque = m_dataMarker;

    // if (m_currentData)
    //     qDebug() << "timestamp=" <<
    //     QDateTime::fromMSecsSinceEpoch(m_currentData->timestamp/1000).toString("hh:mm:ss.zzz")
    //     << "flags=" << m_currentData->flags;

    // Do not display archive in a future
    if (!(m_delegate->getFlags() & QnAbstractArchiveDelegate::Flag_UnsyncTime))
    {
        if (isCompatiblePacketForMask(m_currentData)
            && m_currentData->timestamp > QnSyncTime::currentUSecsSinceEpoch() && !reverseMode)
        {
            m_outOfPlaybackMask = true;
            NX_DEBUG(this, "Media packet in future, return empty packet");
            return createEmptyPacket(reverseMode); // EOF reached
        }
    }

    // ensure Pos At playback mask
    if (!needToStop() && isCompatiblePacketForMask(m_currentData)
        && !(m_currentData->flags & QnAbstractMediaData::MediaFlags_PreRoll))
    {
        m_playbackMaskSync.lock();
        qint64 newTime =
            m_playbackMaskHelper.findTimeAtPlaybackMask(m_currentData->timestamp, !reverseMode);
        m_playbackMaskSync.unlock();

        // The former `newTime > m_delegate->endTime()` end cap was removed:
        // endTime() is the last CLOSED recorded position, so for a live-capable
        // delegate (an archive that tails into the live buffer) it rejected every
        // live-edge frame as "past the end". True end-of-media is already reported
        // when the delegate returns null (createEmptyPacket below), so the cap was
        // redundant for bounded archives and wrong for live ones. The
        // DATETIME_NOW/-1 cases below remain to terminate playback-mask navigation.
        if (newTime == DATETIME_NOW || newTime == -1)
        {
            m_outOfPlaybackMask = true;
            NX_DEBUG(this, "EOF reached(playback mask), return empty packet");
            return createEmptyPacket(reverseMode); // EOF reached
        }

        if (newTime != m_currentData->timestamp)
        {
            if (!exactJumpToSpecifiedFrame && channelCount > 1)
                setNeedKeyData();
            internalJumpTo(newTime);
            m_skipFramesToTime = newTime;
            m_BOF = true;
            goto begin_label;
        }
    }

    if (m_currentData && m_currentData->dataType != QnAbstractMediaData::METADATA)
    {
        if (m_eof)
        {
            m_currentData->flags |= QnAbstractMediaData::MediaFlags_AfterEOF;
            m_eof = false;
        }

        /**
         * Consumers, such as CamDisplay, skip data frames if data with MediaFlags_BOF flag has
         * not been received yet. On the other hand, QnArchiveStreamReader drops non-key
         * frames if it hasn't received one. So, without this condition below
         * (&& m_currentData->isKeyFrame()) we may end up
         * with a frame with MediaFlags_BOF flag being dropped because it is not a key frame and
         * consumer won't never get a BOF frame.
         * Above is true only for the video packets => '!videoData' condition.
         */
        if (m_BOF && (!videoData || m_currentData->isKeyFrame()))
        {
            m_currentData->flags |= QnAbstractMediaData::MediaFlags_BOF;
            m_BOF = false;
        }
    }

    if (m_isStillImage)
        m_currentData->flags |= QnAbstractMediaData::MediaFlags_StillImage;

    if (m_currentData)
    {
        m_latPacketTime = m_currentData->isLive()
            ? DATETIME_NOW
            : qMin(QnSyncTime::currentUSecsSinceEpoch(), m_currentData->timestamp);
    }

    return m_currentData;
}

void QnArchiveStreamReader::internalJumpTo(qint64 usec)
{
    m_afterMotionData.reset();
    qint64 seekRez = 0;
    if (usec > 0)
    {
        seekRez = m_delegate->seek(usec, !m_exactJumpToSpecifiedFrame);
    }
    else
    {
        // some local files can't correctly jump to 0
        m_delegate->close();
        init();
        m_delegate->open(m_url);
    }

    m_exactJumpToSpecifiedFrame = false;
    m_bottomIFrameTime = -1;
    m_lastGopSeekTime = -1;
    m_topIFrameTime = seekRez != -1 ? seekRez : usec;
    m_IFrameAfterJumpFound = false;
    m_eof = false;
    m_afterBOFCounter = -1;
    m_bofReached = false;
}

QnAbstractMediaDataPtr QnArchiveStreamReader::getNextPacket()
{
    QnAbstractMediaDataPtr result;
    while (!needToStop())
    {
        result = m_delegate->getNextData();
        if (result == 0 && !needToStop())
        {
            if (m_cycleMode)
            {
                if (m_delegate->endTime() < 1000 * 1000 * 5)
                    msleep(200); // prevent to fast file walk for very short files.
                m_delegate->close();
                m_eof = true;

                if (!init())
                    return QnAbstractMediaDataPtr();
                result = m_delegate->getNextData();
                if (result == 0)
                    return result;
            }
            else
            {
                m_eof = true;
                NX_DEBUG(this, "EOF from delegate");
                return createEmptyPacket(isReverseMode());
            }
        }

        auto metadata = std::dynamic_pointer_cast<QnAbstractCompressedMetadata>(result);
        if (metadata && metadata->metadataType == MetadataType::object
            && !m_streamDataFilter.testFlag(StreamDataFilter::objects))
        {
            continue;
        }

        if (metadata && metadata->metadataType == MetadataType::heatMap
            && !m_streamDataFilter.testFlag(StreamDataFilter::motion))
        {
            continue;
        }

        break;
    }

    return result;
}

unsigned QnArchiveStreamReader::getCurrentAudioChannel() const
{
    return m_selectedAudioChannel;
}

QStringList QnArchiveStreamReader::getAudioTracksInfo() const
{
    QStringList result;
    for (auto& track: m_delegate->getAudioLayout()->tracks())
        result << track.description;
    return result;
}

bool QnArchiveStreamReader::setAudioChannel(unsigned num)
{
    if (!m_delegate->setAudioChannel(num))
        return false;

    m_selectedAudioChannel = num;
    return true;
}

void QnArchiveStreamReader::setSpeedInternal(double value, qint64 currentTimeHint)
{
    if (value == m_speed)
        return;

    bool oldReverseMode = m_speed;
    bool newReverseMode = value;
    m_speed = value;

    if (oldReverseMode != newReverseMode)
    {
        bool useMutex = !m_externalLocked;
        if (useMutex)
            m_jumpMtx.lock();

        m_currentTimeHint = currentTimeHint;
        if (useMutex)
            m_jumpMtx.unlock();
    }

    m_delegate->beforeChangeSpeed(m_speed);
}

/** Is not used and not implemented. */
bool QnArchiveStreamReader::isNegativeSpeedSupported() const
{
    return true; //! m_delegate->getVideoLayout() || m_delegate->getVideoLayout()->channelCount()
                 //! == 1;
}

bool QnArchiveStreamReader::isSingleShotMode() const
{
    return m_singleShot;
}

void QnArchiveStreamReader::pleaseStop()
{
    base_type::pleaseStop();
    if (m_delegate)
        m_delegate->beforeClose();
    m_singleShowWaitCond.wakeAll();
    m_stopWaitCond.wakeAll();
}

void QnArchiveStreamReader::setEndOfPlaybackHandler(std::function<void()> handler)
{
    m_endOfPlaybackHandler = handler;
    if (m_delegate)
        m_delegate->setEndOfPlaybackHandler(handler);
}

void QnArchiveStreamReader::setErrorHandler(
    std::function<void(const QString& errorString)> handler)
{
    m_errorHandler = handler;
    if (m_delegate)
        m_delegate->setErrorHandler(handler);
}

bool QnArchiveStreamReader::isSkippingFrames() const
{
    NX_MUTEX_LOCKER mutex(&m_jumpMtx);
    return m_skipFramesToTime != 0 || m_tmpSkipFramesToTime != 0;
}

void QnArchiveStreamReader::channeljumpToUnsync(
    qint64 usec,
    int /*channel*/,
    qint64 skipTime,
    bool skipToFrameBefore)
{
    m_singleQuantProcessed = false;
    m_requiredJumpTime = m_lastSeekPosition = usec;
    m_lastUsePreciseSeek = (skipTime != 0);
    m_tmpSkipFramesToTime = skipTime;
    // "Stop before target" mode (set only by previousFrame) is paired with this jump
    // here, alongside the skip target, so it is promoted together on the reader thread.
    m_tmpSkipToFrameBeforeTarget = skipToFrameBefore;
    m_singleShowWaitCond.wakeAll();
}

void QnArchiveStreamReader::directJumpToNonKeyFrame(qint64 usec)
{
    if (usec == qint64(AV_NOPTS_VALUE))
        return;

    bool useMutex = !m_externalLocked;
    if (useMutex)
        m_jumpMtx.lock();

    NX_VERBOSE(
        this,
        "Direct jump to non-key frame to %1",
        nx::utils::timestampToDebugString(usec));
    beforeJumpInternal(usec);
    m_exactJumpToSpecifiedFrame = true;
    channeljumpToUnsync(usec, 0, usec);

    if (useMutex)
        m_jumpMtx.unlock();
}

void QnArchiveStreamReader::setSkipFramesToTime(qint64 skipTime)
{
    m_skipFramesToTime = skipTime;
    emit skipFramesTo(skipTime);

    if (isSingleShotMode())
        QnLongRunnable::resume();
}

bool QnArchiveStreamReader::jumpTo(qint64 usec, qint64 skipTime)
{
    resetRealtimeDelay();
    return jumpToEx(usec, skipTime, true, nullptr);
}

bool QnArchiveStreamReader::jumpTo(qint64 usec, qint64 skipTime, std::optional<int> marker)
{
    resetRealtimeDelay();
    return jumpToEx(
        usec,
        skipTime,
        /*bindPositionToPlaybackMask*/ true,
        /*outJumpTime*/ nullptr,
        marker);
}

bool QnArchiveStreamReader::jumpToEx(
    qint64 usec,
    qint64 skipTime,
    bool bindPositionToPlaybackMask,
    qint64* outJumpTime,
    std::optional<int> marker,
    bool skipToFrameBefore)
{
    NX_VERBOSE(this, "Set position %1 for device %2", usecToDateTime(usec), m_url);

    qint64 newTime = usec;
    if (bindPositionToPlaybackMask)
    {
        m_playbackMaskSync.lock();
        newTime = m_playbackMaskHelper.findTimeAtPlaybackMask(usec, m_speed >= 0);
        m_playbackMaskSync.unlock();
    }

    if (outJumpTime)
        *outJumpTime = newTime;

    if (newTime != usec)
        skipTime = 0;

    bool useMutex = !m_externalLocked;
    if (useMutex)
        m_jumpMtx.lock();

    // Bind the marker to this jump under the same lock that sets the jump time,
    // so the reader thread (which reads both together, see run()) can never pair
    // this marker with a different jump. This is the whole point of carrying the
    // marker into the jump instead of a separate setter.
    if (marker)
        m_newDataMarker = *marker;

    bool usePreciseSeek = (skipTime != 0);
    bool needJump = newTime != m_requiredJumpTime || m_lastUsePreciseSeek != usePreciseSeek;
    if (needJump)
    {
        beforeJumpInternal(newTime);
        channeljumpToUnsync(newTime, 0, skipTime, skipToFrameBefore);
    }

    if (useMutex)
        m_jumpMtx.unlock();

    if (isSingleShotMode())
        QnLongRunnable::resume();
    return needJump;
}

void QnArchiveStreamReader::beforeJumpInternal(qint64 usec)
{
    NX_VERBOSE(this, "Before jump to %1", nx::utils::timestampToDebugString(usec));
    emit beforeJump(usec);
    m_delegate->beforeSeek(usec);
}

bool QnArchiveStreamReader::setStreamDataFilter(StreamDataFilters filter)
{
    if (m_delegate->getFlags() & QnAbstractArchiveDelegate::Flag_CanSendMetadata)
    {
        m_streamDataFilter = filter;
        return true;
    }
    return false;
}

nx::vms::api::StreamDataFilters QnArchiveStreamReader::streamDataFilter() const
{
    return m_streamDataFilter;
}

void QnArchiveStreamReader::setPlaybackRange(const QnTimePeriod& playbackRange)
{
    NX_MUTEX_LOCKER lock(&m_playbackMaskSync);
    m_outOfPlaybackMask = false;
    m_playbackMaskHelper.setPlaybackRange(playbackRange);
}

QnTimePeriod QnArchiveStreamReader::getPlaybackRange() const
{
    return m_playbackMaskHelper.getPlaybackRange();
}

void QnArchiveStreamReader::setPlaybackMask(const QnTimePeriodList& playbackMask)
{
    NX_MUTEX_LOCKER lock(&m_playbackMaskSync);
    m_outOfPlaybackMask = false;
    m_playbackMaskHelper.setPlaybackMask(playbackMask);
}

void QnArchiveStreamReader::setQuality(
    MediaQuality quality,
    bool fastSwitch,
    const QSize& resolution)
{
    if (m_quality != quality || fastSwitch > m_qualityFastSwitch
        || m_customResolution != resolution)
    {
        NX_VERBOSE(
            this,
            "Set quality to %1 (fast %2, resolution %3)",
            quality,
            fastSwitch,
            resolution);

        bool useMutex = !m_externalLocked;
        if (useMutex)
            m_jumpMtx.lock();
        m_quality = quality;
        m_qualityFastSwitch = fastSwitch;
        m_customResolution = resolution;
        if (useMutex)
            m_jumpMtx.unlock();
    }
}

MediaQuality QnArchiveStreamReader::getQuality() const
{
    return m_quality;
}

AVCodecID QnArchiveStreamReader::getTranscodingCodec() const
{
    // TODO: Pass from server. See DEFAULT_VIDEO_CODEC = AV_CODEC_ID_H263P in rtsp_connection.cpp
    return AV_CODEC_ID_H263P;
}

void QnArchiveStreamReader::lock()
{
    m_jumpMtx.lock();
    m_externalLocked =
        true; // external class locks mutex to perform atomic several params changing
}

void QnArchiveStreamReader::unlock()
{
    m_externalLocked = false;
    m_jumpMtx.unlock();
}

void QnArchiveStreamReader::setArchiveDelegate(std::unique_ptr<QnAbstractArchiveDelegate> delegate)
{
    m_delegate = std::move(delegate);
    if (m_endOfPlaybackHandler)
        m_delegate->setEndOfPlaybackHandler(m_endOfPlaybackHandler);

    if (m_errorHandler)
        m_delegate->setErrorHandler(m_errorHandler);
}

QnAbstractArchiveDelegate* QnArchiveStreamReader::getArchiveDelegate() const
{
    return m_delegate.get();
}

std::unique_ptr<QnAbstractArchiveDelegate> QnArchiveStreamReader::releaseArchiveDelegate()
{
    return std::move(m_delegate);
}

void QnArchiveStreamReader::setSpeed(double value, qint64 currentTimeHint)
{
    std::unique_ptr<nx::MutexLocker> lock;
    if (!m_externalLocked)
        lock = std::make_unique<nx::MutexLocker>(&m_jumpMtx, __FILE__, __LINE__);
    m_speed = value;
    auto dataProcessors = m_dataprocessors.lock();
    for (int i = 0; i < dataProcessors->size(); ++i)
    {
        QnAbstractDataConsumer* dp = dynamic_cast<QnAbstractDataConsumer*>(dataProcessors->at(i));
        if (!dp)
            continue;
        dp->setSpeed(value);
    }
    setSpeedInternal(value, currentTimeHint);
}

double QnArchiveStreamReader::getSpeed() const
{
    NX_MUTEX_LOCKER mutex(&m_jumpMtx);
    return m_speed;
}

qint64 QnArchiveStreamReader::startTime() const
{
    NX_ASSERT(m_delegate);
    QnTimePeriod p;
    {
        NX_MUTEX_LOCKER lock(&m_playbackMaskSync);
        p = m_playbackMaskHelper.getPlaybackRange();
    }
    if (p.isEmpty())
        return m_delegate->startTime();
    else
        return p.startTimeMs * 1000;
}

qint64 QnArchiveStreamReader::endTime() const
{
    NX_ASSERT(m_delegate);
    QnTimePeriod p;
    {
        NX_MUTEX_LOCKER lock(&m_playbackMaskSync);
        p = m_playbackMaskHelper.getPlaybackRange();
    }
    if (p.isEmpty())
        return m_delegate->endTime();
    else
        return p.endTimeMs() * 1000;
}

void QnArchiveStreamReader::afterRun()
{
    if (m_delegate)
        m_delegate->close();
}

void QnArchiveStreamReader::setGroupId(const nx::String& guid)
{
    if (m_delegate)
        m_delegate->setGroupId(guid);
}

bool QnArchiveStreamReader::isPaused() const
{
    return base_type::isPaused();
}

void QnArchiveStreamReader::pause()
{
    base_type::pause();
}

void QnArchiveStreamReader::resume()
{
    base_type::resume();
}

bool QnArchiveStreamReader::isRealTimeSource() const
{
    const qint64 requiredJumpTime = m_requiredJumpTime.load();
    return m_delegate && m_delegate->isRealTimeSource()
        && (requiredJumpTime == (qint64) AV_NOPTS_VALUE || requiredJumpTime == DATETIME_NOW);
}

bool QnArchiveStreamReader::needKeyData(int channel) const
{
    if (m_quality == MEDIA_Quality_LowIframesOnly)
        return true;
    return base_type::needKeyData(channel);
}

CameraDiagnostics::Result QnArchiveStreamReader::lastError() const
{
    if (!m_delegate)
        return CameraDiagnostics::NoErrorResult();
    return m_delegate->lastError();
}

bool QnArchiveStreamReader::isReverseMode() const
{
    NX_MUTEX_LOCKER lock(&m_jumpMtx);
    return m_speed < 0;
}

bool QnArchiveStreamReader::isJumpProcessing() const
{
    return m_requiredJumpTime != AV_NOPTS_VALUE;
}

void QnArchiveStreamReader::emitJumpOccurred(qint64 jumpTime, bool usePreciseSeek, int sequence)
{
    {
        NX_MUTEX_LOCKER mutex(&m_jumpMtx);
        if (m_lastUsePreciseSeek == usePreciseSeek && m_requiredJumpTime == jumpTime)
        {
            m_lastUsePreciseSeek = false;
            m_requiredJumpTime = AV_NOPTS_VALUE;
        }
    }
    emit jumpOccurred(jumpTime, sequence);
}

CameraDiagnostics::Result QnArchiveStreamReader::diagnoseMediaStreamConnection()
{
    // TODO/IMPL
    return CameraDiagnostics::Result(CameraDiagnostics::ErrorCode::unknown);
}

void QnArchiveStreamReader::setCycleMode(bool value)
{
    m_cycleMode = value;
}

void QnArchiveStreamReader::jumpToPreviousFrame(qint64 usec)
{
    resetRealtimeDelay();
    // skipToFrameBefore makes the skip loop stop on the frame strictly BEFORE usec
    // (not at/after it, as a precise seek does), so the skip target is usec itself -
    // no usec - 1 boundary fudge is needed.
    if (usec != DATETIME_NOW)
    {
        jumpToEx(
            /*usec*/ qMax(0ll, usec - 200 * 1000),
            /*skipTime*/ usec,
            /*bindPositionToPlaybackMask*/ true,
            /*outJumpTime*/ nullptr,
            /*marker*/ std::nullopt,
            /*skipToFrameBefore*/ true);
    }
    else
    {
        jumpToEx(
            usec,
            /*skipTime*/ 0,
            /*bindPositionToPlaybackMask*/ true,
            /*outJumpTime*/ nullptr,
            /*marker*/ std::nullopt,
            /*skipToFrameBefore*/ true);
    }
}

void QnArchiveStreamReader::addMediaFilter(const std::shared_ptr<AbstractMediaDataFilter>& filter)
{
    m_filters.push_back(filter);
}

void QnArchiveStreamReader::resetRealtimeDelay()
{
    m_sendTimeBaseUs = -1;
}

std::chrono::milliseconds QnArchiveStreamReader::getDelay(int64_t timestamp)
{
    if (!m_realTimeSpeed.has_value())
        return std::chrono::milliseconds();

    if (m_sendTimeBaseUs == -1)
    {
        m_timer.restart();
        m_sendTimeBaseUs = timestamp;
        return std::chrono::milliseconds();
    }

    const int64_t dtUs = (timestamp - m_sendTimeBaseUs) / *m_realTimeSpeed;
    const auto sleepTime = std::chrono::milliseconds(dtUs / 1000) - m_timer.elapsed();
    if (sleepTime > MAX_FRAME_DURATION)
    {
        m_timer.restart();
        m_sendTimeBaseUs = timestamp;
        return std::chrono::milliseconds(); //< Ignore archive gaps.
    }
    if (sleepTime.count() > 0)
        return sleepTime;

    return std::chrono::milliseconds();
}

void QnArchiveStreamReader::applyRealtimeDelay(const QnAbstractMediaDataPtr& data)
{
    if (!data || !m_realTimeSpeed.has_value())
        return;

    // Pre-roll frames after a precise seek (PreRoll) must reach the consumer as
    // fast as possible: the consumer compresses their PTS to 1 ms steps so the
    // browser can flush them through the decoder before showing the seek-target
    // frame. Skipping getDelay here also means m_sendTimeBaseUs stays unset
    // until the first NON-pre-roll frame, so the rate limiter anchors on the
    // seek target instead of on the I-frame that preceded it.
    if (data->flags.testFlag(QnAbstractMediaData::MediaFlags_PreRoll))
        return;

    if (!hasVideo() && data->dataType == QnAbstractMediaData::AUDIO)
        std::this_thread::sleep_for(getDelay(data->timestamp));
    else if (data->dataType == QnAbstractMediaData::VIDEO)
        std::this_thread::sleep_for(getDelay(data->timestamp));
}

void QnArchiveStreamReader::run()
{
    initSystemThreadId();

    NX_VERBOSE(this, "Started");
    beforeRun();

    while (!needToStop())
    {
        pauseDelay(); // pause if needed;
        if (needToStop()) // extra check after pause
            break;

        // check queue sizes
        if (!dataCanBeAccepted())
        {
            resetRealtimeDelay();
            emit waitForDataCanBeAccepted();
            QThread::msleep(10);
            continue;
        }

        QnAbstractMediaDataPtr data = getNextData();

        applyRealtimeDelay(data);

        if (data)
        {
            NX_VERBOSE(
                this,
                "New data, timestamp: %1, type: %2, flags: %3, channel: %4",
                data->timestamp,
                data->dataType,
                data->flags,
                data->channelNumber);
        }
        else
        {
            NX_VERBOSE(this, "Null data");
        }

        for (const auto& filter: m_filters)
        {
            data = std::const_pointer_cast<QnAbstractMediaData>(
                std::dynamic_pointer_cast<const QnAbstractMediaData>(filter->processData(data)));
        }

        if (m_noDataHandler && (!data || data->dataType == QnAbstractMediaData::EMPTY_DATA))
            m_noDataHandler();

        if (!data && !needToStop())
        {
            setNeedKeyData();
            onEvent(CameraDiagnostics::BadMediaStreamResult());

            QThread::msleep(30);
            continue;
        }

        QnCompressedVideoDataPtr videoData =
            std::dynamic_pointer_cast<QnCompressedVideoData>(data);

        if (videoData && videoData->channelNumber > CL_MAX_CHANNEL_NUMBER - 1)
        {
            NX_ASSERT(false);
            continue;
        }

        if (videoData && needKeyData(videoData->channelNumber))
        {
            if (videoData->flags & AV_PKT_FLAG_KEY)
            {
                m_gotKeyFrame.at(videoData->channelNumber)++;
            }
            else
            {
                NX_VERBOSE(
                    this,
                    "Skip non-key frame: %1, type: %2",
                    data->timestamp,
                    data->dataType);
                continue; // need key data but got not key data
            }
        }

        if (data)
        {
            data->dataProvider = this;
            data->deviceId = m_deviceId;
            if (data->flags
                & (QnAbstractMediaData::MediaFlags_AfterEOF | QnAbstractMediaData::MediaFlags_BOF))
            {
                m_stat[data->channelNumber].reset();
            }
        }

        if (!hasVideo())
        {
            if (data)
                m_stat[data->channelNumber].onData(data);
        }
        else
        {
            if (videoData)
                m_stat[data->channelNumber].onData(data);
        }

        putData(std::move(data));
    }

    afterRun();
    NX_VERBOSE(this, "Stopped");
}

void QnArchiveStreamReader::setNoDataHandler(nx::MoveOnlyFunc<void()> noDataHandler)
{
    m_noDataHandler = std::move(noDataHandler);
}
