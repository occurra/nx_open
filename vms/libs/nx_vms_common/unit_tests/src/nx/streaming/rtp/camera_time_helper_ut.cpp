// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include <gtest/gtest.h>

#include <nx/rtp/rtcp.h>
#include <nx/rtp/time_linearizer.h>
#include <nx/streaming/rtp/camera_time_helper.h>

using namespace nx::rtp;
using namespace nx::streaming::rtp;
using namespace std::chrono;

// getTime's last two flags are (isPrimaryStream, isPrimaryTrack): the lane, and whether this is
// the lane's reference track. A camera without UTC time (no Sender Report, no ONVIF extension) is
// handled by the no-UTC path, whose reference track binds to the server clock while the burst
// window re-anchors it every packet. These offset tests exercise the settled behavior after that
// window, so they disable the burst window and keep the original 1000 ms resync threshold.

TEST(CameraTimeHelper, localOffset)
{
    CameraTimeHelper helper(nx::rtp::Sdp::MediaType::Video, "test_resource", nullptr);
    helper.setNoUtcBurstWindow(milliseconds(0));
    helper.setNoUtcResyncThreshold(milliseconds(1000));

    microseconds cameraTime = microseconds(1000);
    microseconds currentTime = microseconds(5000);
    microseconds time = helper.getTime(currentTime, cameraTime, false, false, true);
    ASSERT_EQ(time, microseconds(5000));

    currentTime += microseconds(1300);
    cameraTime += microseconds(1000);
    time = helper.getTime(currentTime, cameraTime, false, false, true);
    ASSERT_EQ(time, microseconds(6000));
}

TEST(CameraTimeHelper, rtpTimeOverflow)
{
    nx::rtp::TimeLinearizer<uint32_t> m_linearizer{0};
    uint32_t rtpTime = 0xfffffff0;
    int64_t reference = rtpTime;
    int64_t time = m_linearizer.linearize(rtpTime);
    ASSERT_EQ(time, rtpTime);

    rtpTime += 20;
    reference += 20;
    time = m_linearizer.linearize(rtpTime);
    ASSERT_EQ(time, reference);

    rtpTime -= 40;
    reference -= 40;
    time = m_linearizer.linearize(rtpTime);
    ASSERT_EQ(time, reference);

    rtpTime += 20;
    reference += 20;
    time = m_linearizer.linearize(rtpTime);
    ASSERT_EQ(time, reference);

    rtpTime += 20;
    reference += 20;
    time = m_linearizer.linearize(rtpTime);
    ASSERT_EQ(time, reference);
}

TEST(CameraTimeHelper, resetTimeOffset)
{
    CameraTimeHelper helper(nx::rtp::Sdp::MediaType::Video, "test_resource", nullptr);
    helper.setNoUtcBurstWindow(milliseconds(0));
    helper.setNoUtcResyncThreshold(milliseconds(1000));

    microseconds cameraTime = microseconds(1000);
    microseconds currentTime = seconds(5);
    microseconds time = helper.getTime(currentTime, cameraTime, false, false, true);
    ASSERT_EQ(time, seconds(5));

    currentTime += seconds(1);
    cameraTime += seconds(3); // rtpTime increase more faster than current time -> reset offset
    time = helper.getTime(currentTime, cameraTime, false, false, true);
    ASSERT_EQ(time, seconds(6));
}

// VMS-125: getTime() must be monotonic per stream. A camera<->system-time resync (bad camera
// clock, or the first-Sender-Report jump from RTP clock to UTC) can otherwise snap the output
// backward, which the MPEG-TS recorder rejects as a non-monotonic DTS and fragments the archive.
// The no-UTC reference track binds to the server clock (burst window kept wide open here), so the
// intended output for each packet is its currentTime, and enforceMonotonic guards it.

TEST(CameraTimeHelper, monotonicNormalAdvanceUnaffected)
{
    // Sanity: a plain forward advance is returned unchanged by the guard.
    CameraTimeHelper helper(nx::rtp::Sdp::MediaType::Video, "test_resource", nullptr);
    helper.setNoUtcBurstWindow(seconds(3600));

    const auto t1 = helper.getTime(milliseconds(1000), milliseconds(1000), false, true, true);
    const auto t2 = helper.getTime(milliseconds(1033), milliseconds(1033), false, true, true);
    ASSERT_EQ(t1, milliseconds(1000));
    ASSERT_EQ(t2, milliseconds(1033));
    ASSERT_GT(t2, t1);
}

TEST(CameraTimeHelper, monotonicClampOnBackwardResync)
{
    // Frame 1 binds output to the server clock (1000ms). Frame 2: the system clock jittered back
    // to 995ms; the server-bound output would follow it backward, so the guard must clamp it
    // forward to prev + 1ms.
    CameraTimeHelper helper(nx::rtp::Sdp::MediaType::Video, "test_resource", nullptr);
    helper.setNoUtcBurstWindow(seconds(3600));

    const auto t1 = helper.getTime(milliseconds(1000), milliseconds(1000), false, true, true);
    ASSERT_EQ(t1, milliseconds(1000));

    const auto t2 = helper.getTime(milliseconds(995), milliseconds(1033), false, true, true);
    ASSERT_EQ(t2, milliseconds(1001)); //< clamped to prev + 1ms, not the backward 995ms.
    ASSERT_GT(t2, t1);
}

TEST(CameraTimeHelper, monotonicNeverReturnsEqual)
{
    // Equal timestamps are also rejected by the muxer, so an identical would-be output is bumped
    // strictly forward.
    CameraTimeHelper helper(nx::rtp::Sdp::MediaType::Video, "test_resource", nullptr);
    helper.setNoUtcBurstWindow(seconds(3600));

    const auto t1 = helper.getTime(milliseconds(2000), milliseconds(2000), false, true, true);
    const auto t2 = helper.getTime(milliseconds(2000), milliseconds(2000), false, true, true);
    ASSERT_EQ(t1, milliseconds(2000));
    ASSERT_EQ(t2, milliseconds(2001));
    ASSERT_GT(t2, t1);
}

TEST(CameraTimeHelper, monotonicRepeatedBackwardStaysIncreasing)
{
    // Sustained backward jitter (system clock stuck behind) each moves the output forward by 1ms;
    // it must remain strictly increasing.
    CameraTimeHelper helper(nx::rtp::Sdp::MediaType::Video, "test_resource", nullptr);
    helper.setNoUtcBurstWindow(seconds(3600));

    auto prev = helper.getTime(milliseconds(1000), milliseconds(1000), false, true, true);
    for (int i = 1; i <= 5; ++i)
    {
        const auto t =
            helper.getTime(milliseconds(990), milliseconds(1000 + i * 33), false, true, true);
        ASSERT_GT(t, prev) << "iteration " << i;
        prev = t;
    }
}

TEST(CameraTimeHelper, monotonicResetForgetsHistory)
{
    // After reset() the guard forgets the last value, so a fresh stream may legitimately jump
    // (e.g. reconnect to a different time base).
    CameraTimeHelper helper(nx::rtp::Sdp::MediaType::Video, "test_resource", nullptr);
    helper.setNoUtcBurstWindow(seconds(3600));

    const auto t1 = helper.getTime(seconds(10), seconds(10), false, true, true);
    ASSERT_EQ(t1, seconds(10));

    helper.reset();

    const auto t2 = helper.getTime(seconds(5), seconds(5), false, true, true); //< lower, allowed.
    ASSERT_EQ(t2, seconds(5));
}

// VMS-446: a camera that provides no UTC time at all is bound to the server clock, its secondary
// lane is bound to its primary lane, and non-reference tracks are passive.

TEST(CameraTimeHelper, noUtcBurstWindowBindsToServerClock)
{
    // A startup burst delivers backlog frames whose camera (RTP) time advances far slower than
    // real time. During the window the anchor is re-derived every packet, so output tracks the
    // server clock instead of baking the stale first frame's offset in for good.
    CameraTimeHelper helper(nx::rtp::Sdp::MediaType::Video, "test_resource", nullptr);
    helper.setNoUtcBurstWindow(seconds(3600));

    const auto t1 = helper.getTime(microseconds(1'000'000), microseconds(0), false, true, true);
    ASSERT_EQ(t1, microseconds(1'000'000));

    // 50 ms of wall time passed but the backlog frame's camera clock advanced only 10 ms.
    const auto t2 =
        helper.getTime(microseconds(1'050'000), microseconds(10'000), false, true, true);
    ASSERT_EQ(
        t2,
        microseconds(1'050'000)); //< server clock, not the 1'010'000 a frozen anchor gives.
}

TEST(CameraTimeHelper, noUtcSelfDriftBoundedByResyncThreshold)
{
    // Past the burst window, a reference track with no primary reference self-binds: its output
    // may drift up to the no-UTC resync threshold from the server clock, then re-anchors.
    CameraTimeHelper helper(nx::rtp::Sdp::MediaType::Video, "test_resource", nullptr);
    helper.setNoUtcBurstWindow(milliseconds(0));
    helper.setNoUtcResyncThreshold(milliseconds(150));

    const auto t1 = helper.getTime(microseconds(1'000'000), microseconds(0), false, true, true);
    ASSERT_EQ(t1, microseconds(1'000'000));

    // Camera clock ran 200 ms while only 50 ms of wall time passed: the 150 ms deviation hits the
    // threshold and the output is re-anchored to the server clock rather than allowed to run away.
    const auto t2 =
        helper.getTime(microseconds(1'050'000), microseconds(200'000), false, true, true);
    ASSERT_EQ(t2, microseconds(1'050'000));
}

TEST(CameraTimeHelper, noUtcNonReferenceTrackIsPassive)
{
    // A non-reference track (audio) rides its own anchored offset: it never resyncs, even when it
    // drifts far past the threshold, and never binds to or publishes into the shared clock.
    auto shared = std::make_shared<TimeOffset>();
    CameraTimeHelper helper(nx::rtp::Sdp::MediaType::Audio, "test_resource", shared);
    helper.setNoUtcResyncThreshold(milliseconds(30));
    helper.setNoUtcBurstWindow(seconds(3600));

    // isPrimaryStream true but isPrimaryTrack false -> passive.
    const auto t1 = helper.getTime(microseconds(1'000'000), microseconds(0), false, true, false);
    ASSERT_EQ(t1, microseconds(1'000'000)); //< anchors to the server clock on the first packet.

    // Camera clock runs 200 ms while only 50 ms of wall time passes: a reference track would
    // resync (150 ms deviation >> 30 ms), but a passive track keeps riding its offset.
    const auto t2 =
        helper.getTime(microseconds(1'050'000), microseconds(200'000), false, true, false);
    ASSERT_EQ(t2, microseconds(1'200'000));

    // A passive track never publishes to the shared clock, even on the primary lane.
    ASSERT_FALSE(shared->primaryAssignedUs.has_value());
}

TEST(CameraTimeHelper, noUtcPrimaryWithinThresholdKeepsCameraTime)
{
    // A reference track whose deviation stays under the resync threshold is not re-anchored: it
    // rides its camera clock rather than snapping to the server clock every packet.
    CameraTimeHelper helper(nx::rtp::Sdp::MediaType::Video, "test_resource", nullptr);
    helper.setNoUtcBurstWindow(milliseconds(0));
    helper.setNoUtcResyncThreshold(milliseconds(150));

    const auto t1 = helper.getTime(microseconds(1'000'000), microseconds(0), false, true, true);
    ASSERT_EQ(t1, microseconds(1'000'000));

    // 100 ms camera advance vs 50 ms wall: the 50 ms deviation is under the 150 ms threshold.
    const auto t2 =
        helper.getTime(microseconds(1'050'000), microseconds(100'000), false, true, true);
    ASSERT_EQ(t2, microseconds(1'100'000)); //< camera-derived, not snapped to 1'050'000.
}

TEST(CameraTimeHelper, noUtcPrimaryPublishesSharedTime)
{
    // The primary lane's reference publishes its assigned time into the shared clock and starts
    // the elapsed timer the secondary lane reconstructs the primary's current time from.
    auto shared = std::make_shared<TimeOffset>();
    ASSERT_FALSE(shared->primaryAssignedUs.has_value());
    ASSERT_FALSE(shared->sincePrimaryPacket.isValid());

    CameraTimeHelper primary(nx::rtp::Sdp::MediaType::Video, "test_resource", shared);
    primary.setNoUtcBurstWindow(seconds(3600));

    const auto t = primary.getTime(microseconds(1'000'000), microseconds(0), false, true, true);
    ASSERT_EQ(t, microseconds(1'000'000));
    ASSERT_EQ(shared->primaryAssignedUs, 1'000'000);
    ASSERT_TRUE(shared->sincePrimaryPacket.isValid());
}

TEST(CameraTimeHelper, noUtcSecondaryFollowsPrimary)
{
    // Two lanes of one camera share the clock. The secondary lane's reference has its own,
    // independent RTP timeline; once it drifts past the strict secondary threshold from the
    // primary lane's reconstructed current time, it is bound back to it so the two lanes agree.
    auto shared = std::make_shared<TimeOffset>();
    CameraTimeHelper primary(nx::rtp::Sdp::MediaType::Video, "test_resource", shared);
    CameraTimeHelper secondary(nx::rtp::Sdp::MediaType::Video, "test_resource", shared);
    for (auto* h: {&primary, &secondary})
    {
        h->setNoUtcBurstWindow(milliseconds(0));
        h->setNoUtcResyncThreshold(milliseconds(150));
        h->setNoUtcSecondaryStreamSyncThreshold(milliseconds(50));
    }

    // Secondary anchors before the primary is known; no reference yet, so it self-binds.
    ASSERT_EQ(
        secondary.getTime(microseconds(1'000'000), microseconds(500'000), false, false, true),
        microseconds(1'000'000));

    // Primary emits and publishes its time (and starts the shared elapsed timer).
    ASSERT_EQ(
        primary.getTime(microseconds(1'000'000), microseconds(0), false, true, true),
        microseconds(1'000'000));

    // The secondary's own camera clock has run far ahead (self-derived 1'300'000). The primary's
    // reconstructed time is ~1'000'000 (almost no real time has elapsed since the primary packet,
    // as the calls are back to back), so the secondary is >> 50 ms away and is pulled back onto
    // the primary's timeline - well below its un-bound 1'300'000.
    const auto t =
        secondary.getTime(microseconds(1'100'000), microseconds(800'000), false, false, true);
    ASSERT_GE(
        t,
        microseconds(1'000'001)); //< bound to the primary (+1us monotonic bump), not 1'300'000.
    ASSERT_LT(t, microseconds(1'200'000));
}

TEST(CameraTimeHelper, noUtcSecondaryWithinThresholdKeepsOwnTime)
{
    // A secondary reference already within the strict threshold of the primary's reconstructed
    // time is left alone - it is not snapped onto the primary.
    auto shared = std::make_shared<TimeOffset>();
    CameraTimeHelper primary(nx::rtp::Sdp::MediaType::Video, "test_resource", shared);
    CameraTimeHelper secondary(nx::rtp::Sdp::MediaType::Video, "test_resource", shared);
    for (auto* h: {&primary, &secondary})
    {
        h->setNoUtcBurstWindow(milliseconds(0));
        h->setNoUtcResyncThreshold(milliseconds(150));
        h->setNoUtcSecondaryStreamSyncThreshold(milliseconds(50));
    }

    ASSERT_EQ(
        primary.getTime(microseconds(1'000'000), microseconds(0), false, true, true),
        microseconds(1'000'000));

    // The secondary's self-derived time is 40 ms above the primary's ~1'000'000 - within the 50 ms
    // threshold, so it keeps its own value instead of being snapped onto the primary.
    const auto t =
        secondary.getTime(microseconds(1'040'000), microseconds(500'000), false, false, true);
    ASSERT_EQ(t, microseconds(1'040'000));
}

TEST(CameraTimeHelper, utcOnceSeenSkipsNoUtcPath)
{
    // Point 3: once any UTC source is seen, the stream stays on the UTC path for its lifetime and
    // never publishes into the no-UTC shared clock, even on a later packet that lacks UTC.
    auto shared = std::make_shared<TimeOffset>();
    CameraTimeHelper helper(nx::rtp::Sdp::MediaType::Video, "test_resource", shared);
    helper.setNoUtcBurstWindow(seconds(3600));

    helper
        .getTime(microseconds(1'000'000), microseconds(1'000'000), /*isUtcTime*/ true, true, true);
    helper.getTime(
        microseconds(1'040'000),
        microseconds(1'040'000),
        /*isUtcTime*/ false,
        true,
        true);

    ASSERT_FALSE(shared->primaryAssignedUs.has_value());
}

TEST(CameraTimeHelper, utcAccurateReturnsCameraTime)
{
    // The UTC path is unchanged: an accurate camera UTC is returned as-is.
    CameraTimeHelper helper(nx::rtp::Sdp::MediaType::Video, "test_resource", nullptr);

    const auto t =
        helper.getTime(microseconds(1'000'000), microseconds(1'000'000), true, true, true);
    ASSERT_EQ(t, microseconds(1'000'000));
}
