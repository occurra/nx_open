// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <optional>

#include <core/resource/resource_media_layout.h>
#include <nx/media/codec_parameters.h>
#include <nx/media/media_data_packet.h>
#include <nx/streaming/abstract_archive_delegate.h>
#include <nx/streaming/abstract_media_stream_data_provider.h>
#include <nx/string.h>
#include <nx/utils/datetime.h>
#include <nx/utils/elapsed_timer.h>
#include <nx/utils/move_only_func.h>
#include <nx/utils/thread/wait_condition.h>
#include <recording/playbackmask_helper.h>

extern "C" {
#include <libavutil/avutil.h>
}

// Private.
class FrameTypeExtractor;

struct QnTimePeriod;
class QnTimePeriodList;
class AbstractMediaDataFilter;

/**
 * Provides reading of media data from QnMediaResource stream with specified playback speed and
 * with navigation supported.
 * Before starting to work, setArchiveDelegate() must be called, passing QnAbstractArchiveDelegate
 * working on the same QnMediaResource. Most of QnAbstractArchiveDelegate implementations
 * require pointer to QnArchiveStreamReader passed into constructor, so cross-referencing will be
 * established this way.
 * To consume data read, user has to subclass QnAbstractDataConsumer and provide one or more
 * instances to addDataProcessor(). Sequential calls for QnAbstractDataConsumer::processData()
 * will happen passing sequential packets of data read in form of QnAbstractMediaDataPtr.
 * Video frames are kept compressed, so decoding is up to user.
 * Allows to set playback speed, including negative, i.e. reverse playback is supported.
 * Navigation supported includes pausing and resuming playback, getting next and previous frames,
 * jumping to specific position (with the most of key frame related routines encapsulated).
 * It is possible to set single (via setPlaybackRange()) or multiple (via setPlaybackMask()) time
 * intervals where playback is exclusively permitted.
 */
class NX_VMS_COMMON_API QnArchiveStreamReader: public QnAbstractMediaStreamDataProvider
{
    Q_OBJECT

    using base_type = QnAbstractMediaStreamDataProvider;

public:
    QnArchiveStreamReader(const nx::Uuid& deviceId, const QString& url);

    virtual ~QnArchiveStreamReader();

    // Manual open. Open will be called automatically on first data access
    bool open();

    bool isSkippingFrames() const;

    // gives a list of audio tracks
    QStringList getAudioTracksInfo() const;

    // gets current audio channel ( if we have the only channel - returns 0 )
    unsigned getCurrentAudioChannel() const;

    // sets certain track
    bool setAudioChannel(unsigned num);

    bool isReverseMode() const;

    /** Is not used and not implemented. */
    bool isNegativeSpeedSupported() const;

    bool isSingleShotMode() const;

    QnConstResourceVideoLayoutPtr getDPVideoLayout() const;
    AudioLayoutConstPtr getDPAudioLayout() const;
    virtual bool hasVideo() const override;

    //! Implementation of QnAbstractMediaStreamDataProvider::diagnoseMediaStreamConnection
    virtual CameraDiagnostics::Result diagnoseMediaStreamConnection() override;

    void setCycleMode(bool value);

    /**
     * Jump to frame directly, ignoring start of GOP, i.e. no preliminary jump to preceding key
     * frame performed, so usually no possibility to correctly decode the frame.
     */
    void directJumpToNonKeyFrame(qint64 usec);

    /** The same as jumpToEx() with bindPositionToPlaybackMask = true. */
    bool jumpTo(qint64 usec, qint64 skipTime);

    /**
     * Same as jumpTo(), but binds marker to this jump atomically (see jumpToEx()
     * marker). Use this instead of a separate marker setter to avoid a race with
     * the reader thread processing the jump.
     */
    bool jumpTo(qint64 usec, qint64 skipTime, std::optional<int> marker);

    void jumpToPreviousFrame(qint64 usec);

    /**
     * Jump to frame at time specified by usec. Actually, preliminary jump to preceding key
     * frame performed, and all intermediate frames are read to allow normal decoding. All such
     * intermediate frames get MediaFlags_PreRoll flag set. It is guaranteed that frames read are
     * enough to decode frames up to including usec.
     * Note: jumping (forward or backward) within the GOP of current position causes
     * preliminary jump to preceding key frame again, despite it is not necessary for decoding.
     * Known bug: requesting a jump to first 1-4 (approximately) frames just after key frame
     * results in actual jump to the key frame and no further. The reason is not known yet.
     * @param usec Time to jump in microseconds.
     * @param skipTime During jump, skip frames until skipTime provided, microseconds. Usual
     *     usage is to pass skipTime equal to usec.
     * @param bindPositionToPlaybackMask Consider time interval provided to setPlaybackRange()
     *     and setPlaybackMask(); i.e. if true, jump outside them is not possible.
     *     If false, jumping outside playback range triggers reading all frames from start
     *     of GOP to the frame specified by usec and pausing the media just after reaching.
     * @param outJumpTime If not nullptr, actual jump time (after correction by playback mask),
     *     in microseconds, will be written by the pointer.
     * @param marker When set, the first packet delivered from this jump (and every packet after
     *     it until the next marked jump) carries it in opaque. Bound to the jump atomically so a
     *     reader-thread seek cannot mis-associate it with a different jump - unlike a separate
     *     setter call.
     * @return false if jump to the same usec and skipTime is already performing right now.
     */
    bool jumpToEx(
        qint64 usec,
        qint64 skipTime,
        bool bindPositionToPlaybackMask,
        qint64* outJumpTime,
        std::optional<int> marker = std::nullopt,
        bool skipToFrameBefore = false);

    void setSkipFramesToTime(qint64 skipTime);
    void nextFrame();
    void needMoreData();
    void previousFrame(qint64 usec);
    void pauseMedia();
    bool isMediaPaused() const;
    void resumeMedia();
    QnAbstractMediaDataPtr getNextData();

    bool setStreamDataFilter(nx::vms::api::StreamDataFilters filter);
    nx::vms::api::StreamDataFilters streamDataFilter() const;

    /**
     * Set time period within the archive, where playback is permitted. Upon reaching borders
     * of playbackRange, playback is paused. If no frames were read by the object yet, also sets
     * current position to the start of playbackRange specified. Other aspects of behavior are the
     * same as for setPlaybackMask().
     */
    void setPlaybackRange(const QnTimePeriod& playbackRange);

    QnTimePeriod getPlaybackRange() const;

    /**
     * Set several time periods within the archive, where playback is permitted. Upon reaching
     * a border of a period, playback is paused. Never changes current playback position.
     * There is the only way to read any frames outside playback mask: call jumpToEx() passing
     * bindPositionToPlaybackMask = false. All other navigation methods will never cause reading
     * frames outside playback mask.
     */
    void setPlaybackMask(const QnTimePeriodList& playbackMask);

    /**
     * @param resolution Should be specified only if quality == MEDIA_Quality_CustomResolution;
     *     width can be <= 0 which is treated as "auto".
     */
    void setQuality(MediaQuality quality, bool fastSwitch, const QSize& resolution = QSize());
    MediaQuality getQuality() const;
    AVCodecID getTranscodingCodec() const;

    void setSpeed(double value, qint64 currentTimeHint = AV_NOPTS_VALUE);
    double getSpeed() const;

    /* For atomic changing several params: quality and position for example */
    void lock();
    void unlock();

    void setArchiveDelegate(std::unique_ptr<QnAbstractArchiveDelegate> delegate);
    QnAbstractArchiveDelegate* getArchiveDelegate() const;
    std::unique_ptr<QnAbstractArchiveDelegate> releaseArchiveDelegate();

    void startPaused(qint64 startTime);

    /**
     * \returns                         Time of the archive's start, in microseconds.
     */
    qint64 startTime() const;

    /**
     * \returns                         Time of the archive's end, in microseconds.
     */
    qint64 endTime() const;

    /* Return true if archive range is accessible immediately without opening an archive */
    bool offlineRangeSupported() const;

    virtual void run() override;
    virtual void afterRun() override;
    void setGroupId(const nx::String& groupId);

    virtual void pause() override;
    virtual void resume() override;
    virtual bool isPaused() const override;

    bool isRealTimeSource() const;
    virtual void pleaseStop() override;

    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool value) { m_enabled = value; }
    void setEndOfPlaybackHandler(std::function<void()> handler);
    void setErrorHandler(std::function<void(const QString& errorString)> handler);
    void setNoDataHandler(nx::MoveOnlyFunc<void()> noDataHandler);

    void addMediaFilter(const std::shared_ptr<AbstractMediaDataFilter>& filter);

    CameraDiagnostics::Result lastError() const;

    /**
     * \returns                         Current position of this reader, in
     *                                  microseconds.
     */
    std::chrono::microseconds currentTime() const;

    void reopen();
    bool isJumpProcessing() const;

    void setRealtimeSpeed(float value) { m_realTimeSpeed = value; }

    // Paces one packet to wall clock at the configured real-time speed: sleeps
    // for getDelay(). No-op when no real-time speed is set (max throughput) or
    // for pre-roll frames. Called from run(); also exposed so a synchronous
    // getNextData() consumer can reuse the same pacing instead of its own.
    void applyRealtimeDelay(const QnAbstractMediaDataPtr& data);

signals:
    void beforeJump(qint64 usec);
    void jumpOccurred(qint64 usec, int sequence);
    void streamAboutToBePaused();
    void streamPaused();
    void streamAboutToBeResumed();
    void streamResumed();
    void nextFrameOccurred();
    void prevFrameOccurred();
    void skipFramesTo(qint64 usec);
    void waitForDataCanBeAccepted();

protected:
    bool init();

    qint64 contentLength() const { return m_delegate->endTime() - m_delegate->startTime(); }
    bool initCodecs();
    bool openFormatContext();
    void setCurrentTime(qint64 value);
    QnAbstractMediaDataPtr createEmptyPacket(bool isReverseMode);
    void beforeJumpInternal(qint64 usec);
    void resetRealtimeDelay();

protected:
    qint64 m_currentTime = 0;
    qint64 m_topIFrameTime = -1;
    qint64 m_bottomIFrameTime = -1;

    int m_primaryVideoIdx = -1;
    int m_audioStreamIndex = -1;

    bool m_firstTime = true;

    qint64 m_tmpSkipFramesToTime = AV_NOPTS_VALUE;
    bool m_cycleMode = false;

private:
    void setSpeedInternal(double speed, qint64 currentTimeHint = AV_NOPTS_VALUE);
    bool isCompatiblePacketForMask(const QnAbstractMediaDataPtr& mediaData) const;
    virtual bool needKeyData(int channel) const override;
    void emitJumpOccurred(qint64 jumpTime, bool usePreciseSeek, int sequence);
    std::chrono::milliseconds getDelay(int64_t timestamp);

private:
    unsigned m_selectedAudioChannel = 0;
    bool m_eof = false;
    FrameTypeExtractor* m_frameTypeExtractor = nullptr;
    qint64 m_lastGopSeekTime = -1;
    bool m_IFrameAfterJumpFound = false;
    std::atomic<qint64> m_requiredJumpTime{AV_NOPTS_VALUE};
    qint64 m_lastSeekPosition = AV_NOPTS_VALUE;
    bool m_lastUsePreciseSeek = false;
    bool m_BOF = false;
    int m_afterBOFCounter = -1;
    int m_dataMarker = 0;
    int m_newDataMarker = 0;
    qint64 m_currentTimeHint = AV_NOPTS_VALUE;
    std::unique_ptr<QnAbstractArchiveDelegate> m_delegate;

private:
    bool m_bofReached = false;
    bool m_externalLocked = false;
    bool m_exactJumpToSpecifiedFrame = false;
    bool m_ignoreSkippingFrame = false;
    qint64 m_skipFramesToTime = 0;
    // Set for a previousFrame() jump: the skip must stop just BEFORE m_skipFramesToTime
    // (not at/after it, as a seek does). It rides the jump like skipTime - passed as a
    // jumpToEx()/channeljumpToUnsync() argument, staged in m_tmp... and promoted to the
    // active m_skipToFrameBeforeTarget when the reader thread applies the jump. Since
    // skip frames are delivered one per call, the frame before the target is already
    // out; the reader just withholds the crossing frame into m_bufferedVideoData and
    // ends the shot, then a following nextFrame() reveals that buffered frame.
    bool m_tmpSkipToFrameBeforeTarget = false;
    bool m_skipToFrameBeforeTarget = false;
    QnAbstractMediaDataPtr m_bufferedVideoData;
    bool m_singleShot = false;
    bool m_singleQuantProcessed = false;
    mutable nx::Mutex m_jumpMtx;
    nx::WaitCondition m_singleShowWaitCond;
    QnAbstractMediaDataPtr m_currentData;
    QnAbstractMediaDataPtr m_afterMotionData;

    mutable nx::Mutex m_playbackMaskSync;
    QnPlaybackMaskHelper m_playbackMaskHelper;
    MediaQuality m_quality = MEDIA_Quality_High;
    bool m_qualityFastSwitch = true;
    QSize m_customResolution;
    MediaQuality m_oldQuality = MEDIA_Quality_High;
    bool m_oldQualityFastSwitch = true;
    QSize m_oldResolution;
    bool m_isStillImage = false;
    double m_speed = 1.0;
    double m_prevSpeed = 1.0;

    bool m_rewSecondaryStarted[CL_MAX_CHANNELS] = {};
    nx::vms::api::StreamDataFilters m_streamDataFilter = nx::vms::api::StreamDataFilter::media;
    nx::vms::api::StreamDataFilters m_prevStreamDataFilter = nx::vms::api::StreamDataFilter::media;

    bool m_outOfPlaybackMask = false;
    qint64 m_latPacketTime = DATETIME_NOW;

    bool m_stopCond = false;
    mutable nx::Mutex m_stopMutex;
    nx::WaitCondition m_stopWaitCond;
    mutable std::optional<bool> m_hasVideo;
    mutable std::optional<AudioLayoutConstPtr> m_audioLayout;

    // For realtime delay.
    std::optional<float> m_realTimeSpeed{};
    nx::utils::ElapsedTimer m_timer; //< Timer for realtime playback delay.
    int64_t m_sendTimeBaseUs = -1;

    bool m_enabled = true;
    std::vector<std::shared_ptr<AbstractMediaDataFilter>> m_filters;
    nx::MoveOnlyFunc<void()> m_noDataHandler;
    const nx::Uuid m_deviceId;

    qint64 determineDisplayTime(bool reverseMode);
    void internalJumpTo(qint64 usec);
    QnAbstractMediaDataPtr getNextPacket();
    void channeljumpToUnsync(
        qint64 usec,
        int channel,
        qint64 skipTime,
        bool skipToFrameBefore = false);
    std::function<void()> m_endOfPlaybackHandler;
    std::function<void(const QString& errorString)> m_errorHandler;
};
