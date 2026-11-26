// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <chrono>
#include <deque>

#include <nx/utils/byte_array.h>

#include "rtcp_nack.h"

namespace nx::rtp {

/**
 * A peer repeats its request for a packet until the packet comes, and on a long path it repeats
 * several times before the first answer can arrive. A packet that was re-sent less than this
 * ago is on its way, so its repeated request gets no second copy.
 */
constexpr std::chrono::milliseconds kRtcpNackMinResendInterval{250};

class NX_RTP_API RtcpNackResponder
{
public:
    using Clock = std::chrono::steady_clock;

    RtcpNackResponder(
        size_t queueLimit = kRtcpNackQueueSize,
        std::chrono::milliseconds minResendInterval = kRtcpNackMinResendInterval);
    void pushPacket(const nx::utils::ByteArray& packet, uint16_t seq);
    void pushRtcpNack(const uint8_t* data, int size);
    bool getNextPacket(nx::utils::ByteArray& outputPacket, Clock::time_point now = Clock::now());

private:
    std::deque<nx::utils::ByteArray> m_packets;
    /** When each of m_packets was last re-sent; the epoch for a packet never re-sent. */
    std::deque<Clock::time_point> m_resentAt;
    std::chrono::milliseconds m_minResendInterval;
    std::deque<uint16_t> m_reports;
    uint16_t m_startSeq = 0;
    size_t m_queueLimit = 0;
};

} // namespace nx::rtp
