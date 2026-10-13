// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include <limits>

#include <gtest/gtest.h>

#include <nx/rtp/rtcp_timestamp_mapper.h>

namespace nx::rtp::test {

namespace {

constexpr int kFrequency = 90000; //< 90kHz video RTP clock.
constexpr int64_t kFrameTicks = 3000; //< 33.33ms per frame at 90kHz (~30fps).
constexpr int64_t kNtpBaseUs = 1'781'245'000'000'000;
constexpr uint32_t kRtpBase = 1'000'000;

// RTP timestamp a Sender Report would carry for a given microsecond instant since rtpBase.
uint32_t rtpAt(int64_t elapsedUs)
{
    return uint32_t(kRtpBase + elapsedUs * kFrequency / 1'000'000);
}

int64_t elapsedAt(int frame)
{
    return frame * kFrameTicks * 1'000'000 / kFrequency;
}

} // namespace

// A jittery Sender Report (its NTP/RTP relationship inconsistent with the previous one) must not
// drag the timestamp backward: the slewed anchor caps how far one report can move the wall-clock
// offset, so the steadily advancing RTP clock keeps the output monotonic.
TEST(RtcpTimestampMapper, JitterDoesNotBreakMonotonicity)
{
    RtcpTimestampMapper mapper;
    mapper.setSmoothingEnabled(true);

    int64_t prev = std::numeric_limits<int64_t>::min();
    for (int frame = 0; frame < 300; ++frame)
    {
        const int64_t elapsedUs = elapsedAt(frame);

        // A well-behaved report keeps NTP and RTP consistent. Every 30th frame, inject a report
        // whose NTP lags by ~800ms - exactly the kind of bad report observed from the camera.
        const bool jittery = frame >= 30 && frame % 30 == 0;

        RtcpSenderReport sr;
        sr.ntpTimestamp = kNtpBaseUs + elapsedUs + (jittery ? -800'000 : 0);
        sr.rtpTimestamp = rtpAt(elapsedUs);

        const int64_t ts =
            mapper.getTimestamp(uint32_t(kRtpBase + frame * kFrameTicks), sr, kFrequency);
        EXPECT_GE(ts, prev) << "Backward timestamp at frame " << frame << " (jittery=" << jittery
                            << ")";
        prev = ts;
    }
}

// A small sustained clock skew (camera slightly faster than the server) must be tracked, not
// ignored: the offset slews toward each report so the error is absorbed continuously instead of
// accumulating into a single large correction. This is the property a plain threshold would lose.
TEST(RtcpTimestampMapper, SlowSkewIsTracked)
{
    RtcpTimestampMapper mapper;

    int64_t firstOffset = 0;
    int64_t lastOffset = 0;
    for (int frame = 0; frame < 100; ++frame)
    {
        const int64_t elapsedUs = elapsedAt(frame);

        RtcpSenderReport sr;
        sr.ntpTimestamp = kNtpBaseUs + elapsedUs + frame * 1'000; //< +1ms/frame skew.
        sr.rtpTimestamp = rtpAt(elapsedUs);

        const int64_t ts =
            mapper.getTimestamp(uint32_t(kRtpBase + frame * kFrameTicks), sr, kFrequency);

        const int64_t offset = ts - (kNtpBaseUs + elapsedUs);
        if (frame == 0)
            firstOffset = offset;
        lastOffset = offset;
    }

    EXPECT_GT(lastOffset - firstOffset, 50'000)
        << "Slow skew was not tracked: offset barely moved";
}

// With smoothing disabled the mapper maps each report directly, so a jittery report moves the
// output backward (legacy behavior). Confirms the on/off switch actually changes behavior.
TEST(RtcpTimestampMapper, SmoothingDisabledMapsDirectly)
{
    RtcpTimestampMapper mapper;
    mapper.setSmoothingEnabled(false);

    RtcpSenderReport sr;
    sr.ntpTimestamp = kNtpBaseUs;
    sr.rtpTimestamp = rtpAt(0);
    const int64_t t0 = mapper.getTimestamp(kRtpBase, sr, kFrequency);

    const int64_t elapsedUs = elapsedAt(1);
    sr.ntpTimestamp = kNtpBaseUs + elapsedUs - 800'000; //< Jittery report.
    sr.rtpTimestamp = rtpAt(elapsedUs);
    const int64_t t1 = mapper.getTimestamp(uint32_t(kRtpBase + kFrameTicks), sr, kFrequency);

    EXPECT_LT(t1, t0) << "With smoothing off, a jittery report should map directly (backward)";
}

// A forward jitter spike (the camera's report jumps the implied offset far ahead, as seen in the
// field up to +1.5s) must also be clamped, so the output advances smoothly rather than leaping
// forward and opening a gap.
TEST(RtcpTimestampMapper, ForwardJitterIsClamped)
{
    RtcpTimestampMapper mapper;

    // Anchor with a clean report.
    RtcpSenderReport sr;
    sr.ntpTimestamp = kNtpBaseUs;
    sr.rtpTimestamp = rtpAt(0);
    const int64_t t0 = mapper.getTimestamp(kRtpBase, sr, kFrequency);

    // Next frame, a report whose NTP leaps 1.5s ahead.
    const int64_t elapsedUs = elapsedAt(1);
    sr.ntpTimestamp = kNtpBaseUs + elapsedUs + 1'500'000;
    sr.rtpTimestamp = rtpAt(elapsedUs);
    const int64_t t1 = mapper.getTimestamp(uint32_t(kRtpBase + kFrameTicks), sr, kFrequency);

    const int64_t deltaUs = t1 - t0;
    EXPECT_GT(deltaUs, 0); //< Still advances.
    // One frame (~33ms) plus at most one slew step (~10ms): nowhere near the 1.5s the report asked
    // for.
    EXPECT_LT(deltaUs, 100'000) << "Forward jitter leaked through instead of being clamped";
}

// The RTP timestamp is 32-bit and wraps roughly every 13 hours at 90kHz. The linearizer must keep
// the output monotonic and smooth across the wrap.
TEST(RtcpTimestampMapper, MonotonicAcrossRtpWraparound)
{
    RtcpTimestampMapper mapper;
    const RtcpSenderReport noSr; //< Exercise the pure linearized-clock path.

    const uint32_t start = 0xFFFFFF00u; //< A few frames before the 32-bit wrap.
    const int64_t expectedStepUs = kFrameTicks * 1'000'000 / kFrequency;

    int64_t prev = mapper.getTimestamp(start, noSr, kFrequency);
    for (int frame = 1; frame < 20; ++frame)
    {
        const uint32_t rtpTime = uint32_t(start + frame * kFrameTicks); //< Wraps mid-loop.
        const int64_t ts = mapper.getTimestamp(rtpTime, noSr, kFrequency);
        EXPECT_GT(ts, prev) << "Backward jump at frame " << frame;
        EXPECT_NEAR(double(ts - prev), double(expectedStepUs), 1.0)
            << "Discontinuity across wrap at frame " << frame;
        prev = ts;
    }
}

// Before any Sender Report arrives, the monotonic linearized RTP clock is used.
TEST(RtcpTimestampMapper, NoSenderReportUsesLinearizedClock)
{
    RtcpTimestampMapper mapper;
    const RtcpSenderReport noSr; //< ntpTimestamp == 0.

    const int64_t t0 = mapper.getTimestamp(kRtpBase, noSr, kFrequency);
    const int64_t t1 = mapper.getTimestamp(uint32_t(kRtpBase + kFrameTicks), noSr, kFrequency);

    EXPECT_GT(t1, t0);
    // Integer division of each timestamp can round independently, so allow 1us of slack.
    EXPECT_NEAR(double(t1 - t0), double(kFrameTicks * 1'000'000 / kFrequency), 1.0);
}

} // namespace nx::rtp::test
