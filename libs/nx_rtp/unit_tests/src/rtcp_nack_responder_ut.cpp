// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include <array>
#include <chrono>

#include <gtest/gtest.h>

#include <nx/rtp/rtcp.h>
#include <nx/rtp/rtcp_nack_responder.h>
#include <nx/utils/time.h>

namespace nx::rtp::test {

using namespace std::chrono;

namespace {

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
int resendCount(RtcpNackResponder& responder, uint16_t seq)
{
    const auto nack = makeNack(seq);
    responder.pushRtcpNack(nack.data(), (int) nack.size());
    int count = 0;
    nx::utils::ByteArray packet;
    while (responder.getNextPacket(packet))
        ++count;
    return count;
}

RtcpNackResponder makeResponder()
{
    RtcpNackResponder responder(/*queueLimit*/ 16);
    responder.setRoundTripTime(250ms);
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

    EXPECT_EQ(resendCount(responder, 105), 1);
}

TEST(RtcpNackResponder, resendsOnEachRequestWithoutARoundTripTime)
{
    RtcpNackResponder responder(/*queueLimit*/ 16);
    nx::utils::ByteArray packet;
    packet.write("payload", 7);
    responder.pushPacket(packet, 100);

    EXPECT_EQ(resendCount(responder, 100), 1);
    EXPECT_EQ(resendCount(responder, 100), 1);
}

TEST(RtcpNackResponder, doesNotResendAPacketThatIsOnItsWay)
{
    nx::utils::test::ScopedSyntheticMonotonicTime time;
    auto responder = makeResponder();

    // A peer on a long path repeats its request every 100 ms, before the first answer arrives.
    EXPECT_EQ(resendCount(responder, 105), 1);
    time.applyRelativeShift(100ms);
    EXPECT_EQ(resendCount(responder, 105), 0);
    time.applyRelativeShift(100ms);
    EXPECT_EQ(resendCount(responder, 105), 0);
}

TEST(RtcpNackResponder, resendsAgainWhenTheAnswerWasLost)
{
    nx::utils::test::ScopedSyntheticMonotonicTime time;
    auto responder = makeResponder();

    EXPECT_EQ(resendCount(responder, 105), 1);
    time.applyRelativeShift(250ms);
    EXPECT_EQ(resendCount(responder, 105), 1);
}

TEST(RtcpNackResponder, tracksEachPacketOnItsOwn)
{
    nx::utils::test::ScopedSyntheticMonotonicTime time;
    auto responder = makeResponder();

    EXPECT_EQ(resendCount(responder, 105), 1);
    time.applyRelativeShift(100ms);
    EXPECT_EQ(resendCount(responder, 106), 1);
}

TEST(RtcpNackResponder, forgetsThePacketsThatLeftTheQueue)
{
    nx::utils::test::ScopedSyntheticMonotonicTime time;
    auto responder = makeResponder();
    EXPECT_EQ(resendCount(responder, 109), 1);

    // Ten more packets push 100..103 out of a queue of 16; 109 stays, and so does its time.
    for (uint16_t seq = 110; seq < 120; ++seq)
    {
        nx::utils::ByteArray packet;
        packet.write("payload", 7);
        responder.pushPacket(packet, seq);
    }

    time.applyRelativeShift(100ms);
    EXPECT_EQ(resendCount(responder, 101), 0);
    EXPECT_EQ(resendCount(responder, 109), 0);
    EXPECT_EQ(resendCount(responder, 115), 1);
}

} // namespace nx::rtp::test
