// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "rtcp_timestamp_mapper.h"

#include <algorithm>

#include <nx/rtp/rtp_ini.h>
#include <nx/utils/log/log.h>

namespace nx::rtp {

namespace {

// Maximum amount the RTCP wall-clock offset may move per Sender Report. Kept below a typical video
// frame interval so the steady forward advance of the RTP clock always dominates a backward slew,
// guaranteeing monotonic output, while still absorbing far more genuine clock skew than any real
// crystal exhibits (10ms/report at ~1 report/s is 10ms/s, vs. ppm-scale real skew).
constexpr int64_t kMaxOffsetSlewUs = 10'000;

} // namespace

RtcpTimestampMapper::RtcpTimestampMapper(std::string logId):
    m_smoothingEnabled(ini().rtcpTimestampSmoothing),
    m_logId(std::move(logId))
{
}

int64_t RtcpTimestampMapper::getTimestamp(
    uint32_t rtpTime,
    const RtcpSenderReport& senderReport,
    int frequency)
{
    // Monotonic RTP clock in microseconds. Computed unconditionally so the linearizer state stays
    // consistent across frames regardless of which timing source is used below.
    const int64_t rtpUs = m_linearizer.linearize(rtpTime) * 1'000'000LL / frequency;

    if (senderReport.ntpTimestamp == 0)
        return rtpUs; //< No Sender Report yet: use the monotonic RTP clock.

    // Offset that maps the monotonic RTP clock onto the camera NTP wall-clock as implied by the
    // current Sender Report. It is (nearly) constant for a given report: the rtpTime terms in
    // srRtpDiffUs and rtpUs cancel, leaving the report's own (ntp - rtp) relationship.
    const int64_t srRtpDiffUs =
        int32_t(rtpTime - senderReport.rtpTimestamp) * 1'000'000LL / frequency;
    const int64_t offsetImplied = int64_t(senderReport.ntpTimestamp) + srRtpDiffUs - rtpUs;

    if (!m_smoothingEnabled)
        return rtpUs + offsetImplied; //< Direct per-report mapping (legacy behavior).

    const bool isNewSenderReport =
        m_activeSrNtp != senderReport.ntpTimestamp || m_activeSrRtp != senderReport.rtpTimestamp;

    if (!m_offsetUs)
    {
        m_offsetUs = offsetImplied; //< First Sender Report: anchor directly.
    }
    else if (isNewSenderReport)
    {
        // Slew toward the new report by a capped step. A jittery report (offset error of hundreds
        // of ms) barely moves the offset and is cancelled by the next report; a small sustained
        // skew is followed in full.
        const int64_t error = offsetImplied - *m_offsetUs;
        const int64_t step = std::clamp<int64_t>(error, -kMaxOffsetSlewUs, kMaxOffsetSlewUs);
        *m_offsetUs += step;
        if (step != error) //< Clamped: the report exceeded one slew step.
        {
            // The report tried to move the wall-clock anchor by more than one slew step - i.e.
            // exactly the kind of bad Sender Report that would otherwise push the timeline
            // backward and freeze the decoder. Clamping it here is what prevents that. Low-rate:
            // fires only on such spikes.
            NX_DEBUG(
                this,
                "RTCP anchor jitter absorbed: report implied %1us offset shift, clamped to %2us",
                error,
                step);
        }
    }
    m_activeSrNtp = senderReport.ntpTimestamp;
    m_activeSrRtp = senderReport.rtpTimestamp;

    return rtpUs + *m_offsetUs;
}

QString RtcpTimestampMapper::idForToStringFromPtr() const
{
    return QString::fromStdString(m_logId);
}

} // namespace nx::rtp
