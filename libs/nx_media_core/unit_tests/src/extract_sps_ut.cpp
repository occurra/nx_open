// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include <nx/codec/h265/extradata.h>
#include <nx/media/codec_parameters.h>
#include <nx/media/h264_utils.h>
#include <nx/media/utils.h>
#include <nx/media/video_data_packet.h>

namespace nx::media::test {

namespace {

// Baseline profile, level 4.1: SPS, PPS and an IDR slice.
const std::vector<uint8_t> kH264AnnexB = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x29, 0xe3, 0x50, 0x14, 0x07, 0xb6, 0x02, 0xdc,
    0x04, 0x04, 0x06, 0x90, 0x78, 0x91, 0x15, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x3c, 0x80,
    0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, 0x00, 0x0c, 0x21, 0x18, 0xa0, 0x00, 0x67,
    0xf9, 0x39, 0x39, 0x39, 0x39, 0x38, 0x8f, 0xd1, 0x1e, 0xa4, 0xe2, 0x35, 0xe9, 0x38};
constexpr int kH264Profile = 66;
constexpr int kH264Level = 41;

// Main profile, level 4.0: VPS, SPS and PPS of a real camera.
const std::vector<uint8_t> kH265AnnexB = {
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x01, 0x60, 0x00, 0x00, 0x03,
    0x00, 0x80, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x78, 0xac, 0x09, 0x00, 0x00, 0x00,
    0x01, 0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x03, 0x00,
    0x00, 0x03, 0x00, 0x78, 0xa0, 0x03, 0xc0, 0x80, 0x10, 0xe7, 0xf8, 0xda, 0xee, 0xd4, 0xdd,
    0xdc, 0x97, 0x58, 0x0b, 0x70, 0x10, 0x10, 0x10, 0x40, 0x00, 0x00, 0xfa, 0x00, 0x00, 0x1d,
    0x4c, 0x1c, 0x87, 0x7b, 0x94, 0x76, 0x20, 0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xc1, 0x72,
    0xb0, 0x9c, 0x14, 0x0a, 0x62, 0x40};
constexpr int kH265Profile = 1;
constexpr int kH265Level = 120;

// A slice in MP4 framing: a 4-byte size, then the NAL unit; it carries no SPS.
const std::vector<uint8_t> kMp4SliceWithoutSps = {
    0x00, 0x00, 0x00, 0x05, 0x65, 0x88, 0x84, 0x00, 0x21};

std::unique_ptr<QnWritableCompressedVideoData> makeFrame(
    AVCodecID codec,
    const std::vector<uint8_t>& data,
    const std::vector<uint8_t>& extradata = {})
{
    auto frame = std::make_unique<QnWritableCompressedVideoData>(data.size());
    frame->compressionType = codec;
    frame->m_data.write((const char*) data.data(), data.size());
    if (!extradata.empty())
    {
        auto context = std::make_shared<CodecParameters>();
        context->setExtradata(extradata.data(), (int) extradata.size());
        frame->context = context;
    }
    return frame;
}

/**
 * Builds an MP4-framed frame that begins with the given bytes: the first 4 of them are the size
 * of its NAL unit, so a head that starts with 1 makes a NAL unit of more than 16 MiB.
 */
std::vector<uint8_t> makeMp4FrameStartingWith(const std::vector<uint8_t>& head)
{
    const uint32_t nalUnitSize =
        (uint32_t(head[0]) << 24) | (head[1] << 16) | (head[2] << 8) | head[3];
    std::vector<uint8_t> frame(4 + nalUnitSize, 0);
    std::copy(head.begin(), head.end(), frame.begin());
    return frame;
}

std::vector<uint8_t> h264Avcc()
{
    return h264::buildExtraDataMp4FromAnnexB(kH264AnnexB.data(), (int) kH264AnnexB.size());
}

std::vector<uint8_t> h265Hvcc()
{
    return h265::buildExtraDataMp4FromAnnexB(kH265AnnexB.data(), (int) kH265AnnexB.size());
}

} // namespace

//-------------------------------------------------------------------------------------------------
// H.264.

TEST(ExtractSpsH264, extradataInAvccFormat)
{
    const auto avcc = h264Avcc();
    ASSERT_EQ(1, avcc[0]);

    h264::SequenceParameterSet sps;
    ASSERT_TRUE(h264::extractSps(avcc.data(), (int) avcc.size(), sps));
    EXPECT_EQ(kH264Profile, sps.profile_idc);
    EXPECT_EQ(kH264Level, sps.level_idc);
}

TEST(ExtractSpsH264, extradataInAnnexBFormat)
{
    h264::SequenceParameterSet sps;
    ASSERT_TRUE(h264::extractSps(kH264AnnexB.data(), (int) kH264AnnexB.size(), sps));
    EXPECT_EQ(kH264Profile, sps.profile_idc);
    EXPECT_EQ(kH264Level, sps.level_idc);
}

TEST(ExtractSpsH264, frameWithAvccContext)
{
    const auto frame = makeFrame(AV_CODEC_ID_H264, kMp4SliceWithoutSps, h264Avcc());

    h264::SequenceParameterSet sps;
    ASSERT_TRUE(h264::extractSps(frame.get(), sps));
    EXPECT_EQ(kH264Profile, sps.profile_idc);
    EXPECT_EQ(kH264Level, sps.level_idc);
}

TEST(ExtractSpsH264, annexBFrameWithoutContext)
{
    const auto frame = makeFrame(AV_CODEC_ID_H264, kH264AnnexB);

    h264::SequenceParameterSet sps;
    ASSERT_TRUE(h264::extractSps(frame.get(), sps));
    EXPECT_EQ(kH264Profile, sps.profile_idc);
    EXPECT_EQ(kH264Level, sps.level_idc);
}

// The frame bytes are a valid avcC record; read as the record they would yield an SPS.
TEST(ExtractSpsH264, mp4FrameStartingWithOneIsNotTakenForAvcc)
{
    const auto data = makeMp4FrameStartingWith(h264Avcc());
    h264::SequenceParameterSet sps;
    ASSERT_TRUE(h264::extractSps(data.data(), (int) data.size(), sps));

    const auto frame = makeFrame(AV_CODEC_ID_H264, data);
    EXPECT_FALSE(h264::extractSps(frame.get(), sps));
}

//-------------------------------------------------------------------------------------------------
// H.265.

TEST(ExtractSpsH265, extradataInHvccFormat)
{
    const auto hvcc = h265Hvcc();
    ASSERT_EQ(1, hvcc[0]);

    h265::SequenceParameterSet sps;
    ASSERT_TRUE(extractSpsH265(hvcc.data(), (int) hvcc.size(), sps));
    EXPECT_EQ(kH265Profile, sps.profile_tier_level.general.profile_idc);
    EXPECT_EQ(kH265Level, sps.profile_tier_level.general.level_idc);
}

TEST(ExtractSpsH265, extradataInAnnexBFormat)
{
    h265::SequenceParameterSet sps;
    ASSERT_TRUE(extractSpsH265(kH265AnnexB.data(), (int) kH265AnnexB.size(), sps));
    EXPECT_EQ(kH265Profile, sps.profile_tier_level.general.profile_idc);
    EXPECT_EQ(kH265Level, sps.profile_tier_level.general.level_idc);
}

TEST(ExtractSpsH265, frameWithHvccContext)
{
    const auto frame = makeFrame(AV_CODEC_ID_H265, kMp4SliceWithoutSps, h265Hvcc());

    h265::SequenceParameterSet sps;
    ASSERT_TRUE(extractSpsH265(frame.get(), sps));
    EXPECT_EQ(kH265Profile, sps.profile_tier_level.general.profile_idc);
    EXPECT_EQ(kH265Level, sps.profile_tier_level.general.level_idc);
}

TEST(ExtractSpsH265, annexBFrameWithoutContext)
{
    const auto frame = makeFrame(AV_CODEC_ID_H265, kH265AnnexB);

    h265::SequenceParameterSet sps;
    ASSERT_TRUE(extractSpsH265(frame.get(), sps));
    EXPECT_EQ(kH265Profile, sps.profile_tier_level.general.profile_idc);
    EXPECT_EQ(kH265Level, sps.profile_tier_level.general.level_idc);
}

// The frame bytes are a valid hvcC record; read as the record they would yield an SPS.
TEST(ExtractSpsH265, mp4FrameStartingWithOneIsNotTakenForHvcc)
{
    const auto data = makeMp4FrameStartingWith(h265Hvcc());
    h265::SequenceParameterSet sps;
    ASSERT_TRUE(extractSpsH265(data.data(), (int) data.size(), sps));

    const auto frame = makeFrame(AV_CODEC_ID_H265, data);
    EXPECT_FALSE(extractSpsH265(frame.get(), sps));
}

} // namespace nx::media::test
