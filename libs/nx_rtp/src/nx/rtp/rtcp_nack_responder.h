// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <chrono>
#include <deque>
#include <optional>

#include <nx/utils/byte_array.h>
#include <nx/utils/elapsed_timer.h>

#include "rtcp_nack.h"

namespace nx::rtp {

class NX_RTP_API RtcpNackResponder
{
public:
    RtcpNackResponder(size_t queueLimit = kRtcpNackQueueSize);
    void pushPacket(const nx::utils::ByteArray& packet, uint16_t seq);
    void pushRtcpNack(const uint8_t* data, int size);
    bool getNextPacket(nx::utils::ByteArray& outputPacket);

    /**
     * Sets the round trip time of the path to the peer. A peer repeats its request for a packet
     * until the packet comes, so the requests that it sent before a re-sent copy reached it
     * arrive within one round trip. Such a request gets no second copy. Without a round trip
     * time each request gets a copy.
     */
    void setRoundTripTime(std::chrono::milliseconds value);

private:
    struct Packet
    {
        nx::utils::ByteArray data;

        /** Runs from the last re-send of the packet; not valid for a packet never re-sent. */
        nx::utils::ElapsedTimer sinceResend;
    };

private:
    std::deque<Packet> m_packets;
    std::optional<std::chrono::milliseconds> m_roundTripTime;
    std::deque<uint16_t> m_reports;
    uint16_t m_startSeq = 0;
    size_t m_queueLimit = 0;
};

} // namespace nx::rtp
