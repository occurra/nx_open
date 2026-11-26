// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include <array>
#include <chrono>

#include <gtest/gtest.h>

#include <nx/rtp/rtcp.h>
#include <nx/rtp/rtcp_nack_responder.h>

namespace nx::rtp::test {

using namespace std::chrono;

namespace {

using Clock = RtcpNackResponder::Clock;

/** A generic NACK that asks for one sequence number. */
std::array<uint8_t, 16> makeNack(uint16_t seq)
{
    return {
        0x81,
        kRtcpGenericFeedback,
        0x0,
        0x3, //< Header, length 3.
        0x1,
        0x2,
        0x3,
        0x4, //< Source SSRC.
        0x5,
        0x6,
        0x7,
        0x8, //< Sender SSRC.
        (uint8_t) (seq >> 8),
        (uint8_t) seq,
        0x0,
        0x0, //< The lost packet, no following ones.
    };
}

/** The count of packets that the responder re-sends for one request of seq. */
int resendCount(RtcpNackResponder& responder, uint16_t seq, Clock::time_point now)
{
    const auto nack = makeNack(seq);
    responder.pushRtcpNack(nack.data(), (int) nack.size());
    int count = 0;
    nx::utils::ByteArray packet;
    while (responder.getNextPacket(packet, now))
        ++count;
    return count;
}

RtcpNackResponder makeResponder()
{
    RtcpNackResponder responder(/*queueLimit*/ 16, /*minResendInterval*/ 250ms);
    for (uint16_t seq = 100; seq < 110; ++seq)
    {
        nx::utils::ByteArray packet;
        packet.write("payload", 7);
        responder.pushPacket(packet, seq);
    }
    return responder;
}

} // namespace

TEST(RtcpNackResponder, resendsARequestedPacket)
{
    auto responder = makeResponder();

    EXPECT_EQ(resendCount(responder, 105, Clock::time_point(1s)), 1);
}

TEST(RtcpNackResponder, doesNotResendAPacketThatIsOnItsWay)
{
    auto responder = makeResponder();
    const Clock::time_point start(1s);

    // A peer on a long path repeats its request every 100 ms, before the first answer arrives.
    EXPECT_EQ(resendCount(responder, 105, start), 1);
    EXPECT_EQ(resendCount(responder, 105, start + 100ms), 0);
    EXPECT_EQ(resendCount(responder, 105, start + 200ms), 0);
}

TEST(RtcpNackResponder, resendsAgainWhenTheAnswerWasLost)
{
    auto responder = makeResponder();
    const Clock::time_point start(1s);

    EXPECT_EQ(resendCount(responder, 105, start), 1);
    EXPECT_EQ(resendCount(responder, 105, start + 250ms), 1);
}

TEST(RtcpNackResponder, tracksEachPacketOnItsOwn)
{
    auto responder = makeResponder();
    const Clock::time_point start(1s);

    EXPECT_EQ(resendCount(responder, 105, start), 1);
    EXPECT_EQ(resendCount(responder, 106, start + 100ms), 1);
}

TEST(RtcpNackResponder, forgetsThePacketsThatLeftTheQueue)
{
    auto responder = makeResponder();
    const Clock::time_point start(1s);
    EXPECT_EQ(resendCount(responder, 109, start), 1);

    // Ten more packets push 100..103 out of a queue of 16; 109 stays, and so does its time.
    for (uint16_t seq = 110; seq < 120; ++seq)
    {
        nx::utils::ByteArray packet;
        packet.write("payload", 7);
        responder.pushPacket(packet, seq);
    }

    EXPECT_EQ(resendCount(responder, 101, start + 100ms), 0);
    EXPECT_EQ(resendCount(responder, 109, start + 100ms), 0);
    EXPECT_EQ(resendCount(responder, 115, start + 100ms), 1);
}

} // namespace nx::rtp::test
