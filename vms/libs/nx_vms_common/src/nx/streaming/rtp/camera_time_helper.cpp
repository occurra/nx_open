// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "camera_time_helper.h"

#include <nx/streaming/nx_streaming_ini.h>
#include <nx/utils/log/log.h>
#include <utils/common/synctime.h>
#include <utils/common/util.h>

namespace nx::streaming::rtp {

using namespace std::chrono;

namespace {

constexpr auto kMaxAdjustmentHistoryRecordAge = 30s;
constexpr auto kWaitSenderReportThreshold = 20s;

} // namespace

CameraTimeHelper::CameraTimeHelper(
    nx::rtp::Sdp::MediaType mediaType,
    const std::string& resourceId,
    const TimeOffsetPtr& offset):
    m_primaryOffset(offset),
    m_resyncThreshold(milliseconds(nxStreamingIni().resyncThresholdMs)),
    m_noUtcResyncThreshold(milliseconds(nxStreamingIni().noUtcResyncThresholdMs)),
    m_noUtcSecondaryStreamSyncThreshold(
        milliseconds(nxStreamingIni().noUtcSecondaryStreamSyncThresholdMs)),
    m_noUtcBurstWindow(milliseconds(nxStreamingIni().noUtcBurstWindowMs)),
    m_streamsSyncThreshold(milliseconds(nxStreamingIni().streamsSyncThresholdMs)),
    m_forceCameraTimeThreshold(
        mediaType == nx::rtp::Sdp::MediaType::Metadata
            ? milliseconds(nxStreamingIni().cameraMetadataTimeThresholdMs)
            : milliseconds(nxStreamingIni().forceCameraTimeThresholdMs)),
    m_maxExpectedMetadataDelay(milliseconds(nxStreamingIni().maxExpectedMetadataDelayMs)),
    m_resourceId(resourceId),
    m_mediaType(mediaType),
    m_adjustmentHistory(kMaxAdjustmentHistoryRecordAge)
{
    m_waitSenderReportTimer.restart();
}

void CameraTimeHelper::setTimePolicy(TimePolicy policy)
{
    m_timePolicy = policy;

    // Reset bad camera state if "Trust camera time" re-enabled.
    if (m_timePolicy == TimePolicy::useCameraTimeIfCorrect)
        m_badCameraTimeState = false;
}

void CameraTimeHelper::reset()
{
    m_localOffset.initialized = false;
    m_adjustmentHistory.reset();
    m_lastReturnedTime.reset();
    m_everSawUtc = false;
}

void CameraTimeHelper::stopWaitingSenderReport()
{
    m_waitSenderReportTimer.invalidate();
}

std::chrono::microseconds CameraTimeHelper::replayAdjustmentFromHistory(
    std::chrono::microseconds cameraTimestamp)
{
    const auto latestHistoryTimestamp = m_adjustmentHistory.getLatestSourceTimestamp();
    if (latestHistoryTimestamp
        && *latestHistoryTimestamp - kMaxAdjustmentHistoryRecordAge <= cameraTimestamp
        && cameraTimestamp <= *latestHistoryTimestamp + m_forceCameraTimeThreshold)
    {
        return m_adjustmentHistory.replay(cameraTimestamp);
    }

    const auto vmsSystemTimestamp = QnSyncTime::currentTimePoint();
    const auto lowBound =
        vmsSystemTimestamp - m_forceCameraTimeThreshold - m_maxExpectedMetadataDelay;
    const auto highBound = vmsSystemTimestamp + m_forceCameraTimeThreshold;
    if (lowBound < cameraTimestamp && cameraTimestamp < highBound)
        return cameraTimestamp;

    NX_MUTEX_LOCKER locker(&m_historicalAdjustmentDeltaMutex);
    if (!m_historicalAdjustmentDelta)
        m_historicalAdjustmentDelta = vmsSystemTimestamp - cameraTimestamp;

    return cameraTimestamp + *m_historicalAdjustmentDelta;
}

microseconds CameraTimeHelper::getTime(
    const microseconds currentTime,
    const microseconds cameraTime,
    const bool isUtcTime,
    const bool isPrimaryStream,
    const bool isPrimaryTrack,
    EventCallback callback)
{
    if (isUtcTime)
        m_everSawUtc = true;

    // A camera that provides no UTC time at all - no RTCP Sender Report and no ONVIF timestamp
    // extension - carries only an arbitrary RTP clock. Bind it to the server clock ourselves,
    // and bind the secondary lane to the primary lane so both lanes of the camera agree (see
    // assignNoUtcTime). Once any UTC source has ever been seen we stay on the UTC path below for
    // the life of the stream; the helper is recreated (m_everSawUtc cleared) on reopen.
    if (m_timePolicy != TimePolicy::forceCameraTime && !isUtcTime && !m_everSawUtc)
        return assignNoUtcTime(currentTime, cameraTime, isPrimaryStream, isPrimaryTrack);

    // The shared reference clock is owned by the primary lane's reference track; every other
    // track and the secondary lane bind to it.
    const bool isReference = isPrimaryStream && isPrimaryTrack;

    bool isCameraTimeAccurate = abs(cameraTime - currentTime) < m_forceCameraTimeThreshold;
    if (m_timePolicy == TimePolicy::forceCameraTime)
    {
        if (isReference)
            m_adjustmentHistory.record(cameraTime, 0us);
        return enforceMonotonic(cameraTime);
    }

    if (m_timePolicy == TimePolicy::useCameraTimeIfCorrect)
    {
        if (isUtcTime && isCameraTimeAccurate)
        {
            if (m_badCameraTimeState)
            {
                NX_DEBUG(this, "Camera time is back to normal, camera time will used");
                callback(EventType::CameraTimeBackToNormal);
            }
            m_badCameraTimeState = false;
            if (isReference)
                m_adjustmentHistory.record(cameraTime, 0us);
            NX_VERBOSE(
                this,
                "ResourceId: %1, camera time is accurate: %2ms, system time: %3ms, dt: %4ms",
                getName(),
                cameraTime.count() / 1000,
                currentTime.count() / 1000,
                (cameraTime - currentTime).count() / 1000);
            return enforceMonotonic(cameraTime);
        }
        else if (
            !m_badCameraTimeState
            && m_waitSenderReportTimer.hasExpired(kWaitSenderReportThreshold))
        {
            NX_DEBUG(
                this,
                "ResourceId: %1, camera time is not accurate: %2ms, system time: %3ms, dt: %4ms"
                ", system time will used",
                getName(),
                cameraTime.count() / 1000,
                currentTime.count() / 1000,
                (cameraTime - currentTime).count() / 1000);
            if (isReference)
                callback(EventType::BadCameraTime);

            m_badCameraTimeState = true;
        }
    }

    bool hasPrimaryOffset = m_primaryOffset && m_primaryOffset->initialized;
    bool isStreamDesync = !isReference && hasPrimaryOffset
        && abs(m_primaryOffset->value.load() + cameraTime - currentTime) > m_streamsSyncThreshold;

    if (isStreamDesync)
        callback(EventType::StreamDesync);

    bool useLocalOffset = !isUtcTime || // no UTC time -> can not sync streams
        isStreamDesync || // streams desync exceeds the threshold
        !m_primaryOffset || // primary offset not used
        (!hasPrimaryOffset && !isReference); // reference track not init offset yet

    TimeOffset& offset = useLocalOffset ? m_localOffset : *m_primaryOffset;
    if (!offset.initialized)
    {
        offset.initialized = true;
        offset.value = currentTime - cameraTime;
    }

    const microseconds deviation = abs(offset.value.load() + cameraTime - currentTime);

    NX_VERBOSE(
        this,
        "Camera time: %1, vms time: %2, dt: %3ms, deviation: %4, offset: %5, isReference: %6, useLocalOffset: %7, resyncThreshold: %8",
        std::chrono::duration_cast<std::chrono::milliseconds>(cameraTime),
        std::chrono::duration_cast<std::chrono::milliseconds>(currentTime),
        (cameraTime - currentTime).count() / 1000,
        std::chrono::duration_cast<std::chrono::milliseconds>(deviation),
        std::chrono::duration_cast<std::chrono::milliseconds>(offset.value.load()),
        isReference,
        useLocalOffset,
        m_resyncThreshold);

    if ((isReference || useLocalOffset) && deviation >= m_resyncThreshold)
    {
        NX_DEBUG(
            this,
            "ResourceId: %1. Resync camera time, deviation %2ms, cameraTime: %3ms, isReference %4, useLocalOffset %5",
            getName(),
            deviation.count() / 1000,
            cameraTime.count() / 1000,
            isReference,
            useLocalOffset);
        offset.value = currentTime - cameraTime;
        if (isReference)
            callback(EventType::ResyncToLocalTime);
    }
    const auto offsetValue = offset.value.load();
    if (isReference)
        m_adjustmentHistory.record(cameraTime, offsetValue);
    return enforceMonotonic(offsetValue + cameraTime);
}

microseconds CameraTimeHelper::assignNoUtcTime(
    microseconds currentTime,
    microseconds cameraTime,
    bool isPrimaryStream,
    bool isPrimaryTrack)
{
    // First packet on this track: anchor the local mapping and start the burst window.
    if (!m_localOffset.initialized)
    {
        m_localOffset.initialized = true;
        m_localOffset.value = currentTime - cameraTime;
        m_noUtcAnchorTimer.restart();
    }

    // A non-reference track (audio) is passive: it rides its own anchored offset, with no burst
    // re-anchor, resync or cross-lane binding. Only the lane's reference track carries the clock.
    if (!isPrimaryTrack)
        return enforceMonotonic(m_localOffset.value.load() + cameraTime);

    // Startup burst: a camera often dumps its buffered GOP backlog as a fast burst right after
    // the stream opens. Those frames carry old RTP times but arrive now, so anchoring on the
    // first one bakes the burst depth into the offset for good. Re-derive the anchor on every
    // packet during the window so the newest, real-time frame wins it.
    if (m_noUtcAnchorTimer.isValid() && m_noUtcAnchorTimer.elapsed() < m_noUtcBurstWindow)
        m_localOffset.value = currentTime - cameraTime;

    microseconds assigned = m_localOffset.value.load() + cameraTime;

    if (isPrimaryStream)
    {
        // The primary lane's reference. Keep it within the no-UTC resync threshold of the server
        // clock, then publish it so the secondary lane can follow.
        if (abs(assigned - currentTime) >= m_noUtcResyncThreshold)
        {
            m_localOffset.value = currentTime - cameraTime;
            assigned = currentTime;
        }
        if (m_primaryOffset)
        {
            NX_MUTEX_LOCKER lock(&m_primaryOffset->primaryMutex);
            m_primaryOffset->primaryAssignedUs = assigned.count();
            m_primaryOffset->sincePrimaryPacket.restart();
        }
    }
    else
    {
        // The secondary lane's reference. Follow the primary lane's reconstructed current time so
        // both lanes agree - which is what keeps overlay metadata produced on one lane aligned
        // with video viewed on the other.
        std::optional<int64_t> primaryAssigned;
        std::optional<microseconds> sincePrimary;
        if (m_primaryOffset)
        {
            NX_MUTEX_LOCKER lock(&m_primaryOffset->primaryMutex);
            primaryAssigned = m_primaryOffset->primaryAssignedUs;
            sincePrimary = m_primaryOffset->sincePrimaryPacket.maybeElapsed<microseconds>();
        }
        if (primaryAssigned && sincePrimary)
        {
            const microseconds expected = microseconds(*primaryAssigned) + *sincePrimary;
            if (abs(assigned - expected) > m_noUtcSecondaryStreamSyncThreshold)
            {
                m_localOffset.value = expected - cameraTime;
                assigned = expected;
            }
        }
        else if (abs(assigned - currentTime) >= m_noUtcResyncThreshold)
        {
            // No primary reference yet - self-bind to the server clock until it appears.
            m_localOffset.value = currentTime - cameraTime;
            assigned = currentTime;
        }
    }

    return enforceMonotonic(assigned);
}

std::chrono::microseconds CameraTimeHelper::enforceMonotonic(std::chrono::microseconds result)
{
    if (m_lastReturnedTime && result <= *m_lastReturnedTime)
    {
        const auto clamped = *m_lastReturnedTime + 1ms;
        NX_DEBUG(
            this,
            "ResourceId: %1, non-monotonic time clamped forward: %2ms -> %3ms (raw was %4us behind)",
            getName(),
            result.count() / 1000,
            clamped.count() / 1000,
            (*m_lastReturnedTime - result).count());
        result = clamped;
    }
    m_lastReturnedTime = result;
    return result;
}

std::string CameraTimeHelper::getName() const
{
    return nx::format("%1(%2)", m_resourceId, toString(m_mediaType)).toStdString();
}

} // namespace nx::streaming::rtp
