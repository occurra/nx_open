// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <QtCore/QString>

#include <nx/rtp/rtcp.h>
#include <nx/rtp/rtp.h>
#include <nx/rtp/time_linearizer.h>

namespace nx::rtp {

/**
 * Maps the RTP clock of a single media track onto an absolute timestamp in microseconds.
 *
 * When RTCP Sender Reports are available the RTP clock is mapped onto the camera NTP wall-clock.
 * Re-anchoring to a fresh Sender Report is slew-rate limited: the offset moves toward each new
 * report by at most kMaxOffsetSlewUs, so a jittery or inaccurate report cannot push the resulting
 * timestamp backward (which freezes decoders), while a genuine slow clock skew is still tracked
 * smoothly. The steady forward advance of the RTP clock dominates the capped backward slew, so the
 * output is monotonic by construction. Smoothing defaults to the nx_rtp.ini rtcpTimestampSmoothing
 * flag and can be overridden with setSmoothingEnabled; when off, each Sender Report is mapped
 * directly.
 *
 * Without Sender Reports the linearized RTP clock is returned.
 *
 * Not thread-safe: a mapper instance belongs to one track and is driven from that track's thread.
 */
class NX_RTP_API RtcpTimestampMapper
{
public:
    explicit RtcpTimestampMapper(std::string logId = {});

    /**
     * @param rtpTime Raw RTP timestamp of the current frame.
     * @param senderReport Latest received Sender Report, or a default-constructed one
     * (ntpTimestamp 0) when none has arrived yet.
     * @param frequency RTP clock rate in Hz (e.g. 90000 for video).
     * @return Absolute timestamp in microseconds.
     */
    int64_t getTimestamp(uint32_t rtpTime, const RtcpSenderReport& senderReport, int frequency);

    void setSmoothingEnabled(bool value) { m_smoothingEnabled = value; }

    QString idForToStringFromPtr() const;

private:
    TimeLinearizer<uint32_t> m_linearizer{0};
    std::optional<int64_t> m_offsetUs;
    std::optional<uint64_t> m_activeSrNtp;
    std::optional<uint32_t> m_activeSrRtp;
    bool m_smoothingEnabled = true;
    std::string m_logId;
};

} // namespace nx::rtp
