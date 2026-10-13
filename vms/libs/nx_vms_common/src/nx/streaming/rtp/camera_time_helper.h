// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <nx/rtp/sdp.h>
#include <nx/utils/elapsed_timer.h>
#include <nx/utils/timestamp_adjustment_history.h>

namespace nx::streaming::rtp {

enum class TimePolicy
{
    bindCameraTimeToLocalTime, //< Use camera time, bound to the server time.
    forceCameraTime, //< Use camera time only.
    useCameraTimeIfCorrect, //< Use camera time if it's close to the server time.
};

struct TimeOffset
{
    std::atomic<bool> initialized{false};
    std::atomic<std::chrono::microseconds> value{std::chrono::microseconds(0)};

    // Cross-lane synchronization for a camera that provides no UTC time (no RTCP Sender Report,
    // no ONVIF timestamp extension). The object is shared between a camera's primary and
    // secondary stream providers. The primary lane publishes the time it last assigned and
    // restarts the timer; a dependent lane reconstructs the primary's current time as
    // primaryAssignedUs + sincePrimaryPacket.elapsed() and binds itself to it, so both lanes
    // agree. Guarded by primaryMutex; primaryAssignedUs is empty and the timer is invalid until
    // the primary lane has emitted a frame.
    mutable nx::Mutex primaryMutex;
    std::optional<int64_t> primaryAssignedUs;
    nx::utils::ElapsedTimer sincePrimaryPacket;
};

using TimeOffsetPtr = std::shared_ptr<TimeOffset>;

class NX_VMS_COMMON_API CameraTimeHelper
{
public:
    enum class EventType
    {
        BadCameraTime,
        CameraTimeBackToNormal,
        StreamDesync,
        ResyncToLocalTime,
    };

    using EventCallback = std::function<void(EventType)>;

public:
    CameraTimeHelper(
        nx::rtp::Sdp::MediaType mediaType,
        const std::string& resourceId,
        const TimeOffsetPtr& offset);

    std::chrono::microseconds getTime(
        const std::chrono::microseconds currentTime,
        const std::chrono::microseconds cameraTime,
        const bool isUtcTime,
        const bool isPrimaryStream,
        const bool isPrimaryTrack,
        EventCallback callback = [](CameraTimeHelper::EventType /*event*/) {});

    void setResyncThreshold(std::chrono::milliseconds value) { m_resyncThreshold = value; }
    void setNoUtcResyncThreshold(std::chrono::milliseconds value)
    {
        m_noUtcResyncThreshold = value;
    }
    void setNoUtcSecondaryStreamSyncThreshold(std::chrono::milliseconds value)
    {
        m_noUtcSecondaryStreamSyncThreshold = value;
    }
    void setNoUtcBurstWindow(std::chrono::milliseconds value) { m_noUtcBurstWindow = value; }
    void setTimePolicy(TimePolicy policy);
    void reset();
    void stopWaitingSenderReport();

    std::chrono::microseconds replayAdjustmentFromHistory(
        std::chrono::microseconds cameraTimestamp);

private:
    std::string getName() const;

    // Assigns a timestamp for a camera that provides no UTC time. A non-reference track is passive
    // (rides its own anchored offset). The reference track is bound to the server clock: the
    // primary lane's reference publishes it (kept within m_noUtcResyncThreshold), the secondary
    // lane's reference follows the primary's reconstructed time (within
    // m_noUtcSecondaryStreamSyncThreshold). See TimeOffset.
    std::chrono::microseconds assignNoUtcTime(
        std::chrono::microseconds currentTime,
        std::chrono::microseconds cameraTime,
        bool isPrimaryStream,
        bool isPrimaryTrack);

private:
    TimeOffsetPtr m_primaryOffset;
    TimeOffset m_localOffset; //< used in case when no rtcp reports
    std::chrono::milliseconds m_resyncThreshold;
    std::chrono::milliseconds m_noUtcResyncThreshold;
    std::chrono::milliseconds m_noUtcSecondaryStreamSyncThreshold;
    std::chrono::milliseconds m_noUtcBurstWindow;
    const std::chrono::milliseconds m_streamsSyncThreshold;
    const std::chrono::milliseconds m_forceCameraTimeThreshold;
    const std::chrono::milliseconds m_maxExpectedMetadataDelay;
    nx::utils::ElapsedTimer
        m_waitSenderReportTimer; //< need to wait Sender Report before sending BadCameraTime event
    nx::utils::ElapsedTimer
        m_noUtcAnchorTimer; //< times the startup-burst window on the no-UTC path
    const std::string m_resourceId;
    const nx::rtp::Sdp::MediaType m_mediaType;
    TimePolicy m_timePolicy = TimePolicy::useCameraTimeIfCorrect;
    bool m_badCameraTimeState = false;
    // Latched once a Sender Report or ONVIF UTC timestamp is seen: the stream then stays on the
    // UTC path for its lifetime and never reverts to the no-UTC path. Cleared on reset()/reopen.
    bool m_everSawUtc = false;
    nx::utils::TimestampAdjustmentHistory m_adjustmentHistory;
    mutable nx::Mutex m_historicalAdjustmentDeltaMutex;
    std::optional<std::chrono::microseconds> m_historicalAdjustmentDelta;

    // Enforces monotonic output per stream (VMS-125): getTime() must never return a value below
    // the previous one for this track. A camera<->system-time resync (e.g. when the camera clock
    // is wildly off, or at the first-Sender-Report jump from RTP clock to UTC) can otherwise snap
    // the result backward, which the MPEG-TS recorder rejects as a non-monotonic DTS and fragments
    // the archive. The offset/resync logic is unchanged; only the final output is clamped forward.
    std::optional<std::chrono::microseconds> m_lastReturnedTime;
    std::chrono::microseconds enforceMonotonic(std::chrono::microseconds result);
};

} // namespace nx::streaming::rtp
