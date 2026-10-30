// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include <gtest/gtest.h>

#include <QtCore/QtEndian>

#include <nx/media/video_data_packet.h>
#include <nx/webrtc/webrtc_data_channel_video.h>

namespace nx::webrtc::test {

namespace {

QnConstAbstractMediaDataPtr makeH264Packet(int width, int height, bool isKeyFrame)
{
    auto context = std::make_shared<CodecParameters>();
    auto params = context->getAvCodecParameters();
    params->codec_id = AV_CODEC_ID_H264;
    params->width = width;
    params->height = height;
    auto packet = std::make_shared<QnWritableCompressedVideoData>(/*capacity*/ 0, context);
    packet->compressionType = AV_CODEC_ID_H264;
    if (isKeyFrame)
        packet->flags |= QnAbstractMediaData::MediaFlags_AVKey;
    return packet;
}

} // namespace

TEST(DataChannelVideo, refusesH264AboveTheLevel52FrameSize)
{
    EXPECT_FALSE(DataChannelVideo::isRefusedByWebRtc(makeH264Packet(3840, 2160, true)));
    EXPECT_FALSE(DataChannelVideo::isRefusedByWebRtc(makeH264Packet(4096, 2304, true)));
    EXPECT_TRUE(DataChannelVideo::isRefusedByWebRtc(makeH264Packet(4096, 2320, true)));
    EXPECT_TRUE(DataChannelVideo::isRefusedByWebRtc(makeH264Packet(4000, 3000, false)));
    EXPECT_TRUE(DataChannelVideo::isRefusedByWebRtc(makeH264Packet(8704, 64, true)));
}

TEST(DataChannelVideo, theMessageCarriesTheFrameAndItsAccessUnit)
{
    DataChannelVideo::Frame frame;
    frame.deviceId = nx::Uuid::createUuid();
    frame.index = 7;
    frame.rtpTimestamp = 0xFFFFFFF0;
    frame.timestampUs = 1761740000123456;
    frame.flags = QnAbstractMediaData::MediaFlags_AVKey | QnAbstractMediaData::MediaFlags_PreRoll;
    frame.width = 5968;
    frame.height = 2688;

    std::vector<uint8_t> data(100 * 1024);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = (uint8_t) i;

    const std::string message =
        DataChannelVideo::makeMessage(frame, data.data(), data.size()).toStdString();
    ASSERT_EQ(message.size(), DataChannelVideo::kHeaderSize + data.size());
    EXPECT_EQ(message.substr(0, 4), "OCDV");
    EXPECT_EQ(message[4], (char) DataChannelVideo::kVersion);
    EXPECT_EQ(message[5], (char) DataChannelVideo::kCodecH264);
    EXPECT_EQ(
        qFromBigEndian<uint32_t>(message.data() + 8),
        (uint32_t) (QnAbstractMediaData::MediaFlags_AVKey
            | QnAbstractMediaData::MediaFlags_PreRoll));
    EXPECT_EQ(nx::Uuid::fromRfc4122(QByteArray(message.data() + 12, 16)), frame.deviceId);
    EXPECT_EQ(qFromBigEndian<uint32_t>(message.data() + 28), 7U);
    EXPECT_EQ(qFromBigEndian<uint32_t>(message.data() + 32), 0xFFFFFFF0U);
    EXPECT_EQ(qFromBigEndian<int64_t>(message.data() + 36), 1761740000123456);
    EXPECT_EQ(qFromBigEndian<uint16_t>(message.data() + 44), 5968);
    EXPECT_EQ(qFromBigEndian<uint16_t>(message.data() + 46), 2688);
    EXPECT_EQ(
        std::string_view(message).substr(DataChannelVideo::kHeaderSize),
        std::string_view((const char*) data.data(), data.size()));
}

TEST(DataChannelVideo, theMessageCarriesTheFrameSizeOfTheCodecParameters)
{
    DataChannelVideo dataChannelVideo;
    dataChannelVideo.setDataPacket(makeH264Packet(5968, 2688, true));
    nx::Buffer message;
    ASSERT_TRUE(dataChannelVideo.getNextPacket(message));
    EXPECT_EQ(qFromBigEndian<uint16_t>(message.data() + 44), 5968);
    EXPECT_EQ(qFromBigEndian<uint16_t>(message.data() + 46), 2688);
}

TEST(DataChannelVideo, aSizePrefixedUnitThatLooksLikeAStartCodeIsConverted)
{
    // avcC extradata: the unit is size-prefixed whatever its first bytes look like.
    auto context = std::make_shared<CodecParameters>();
    context->getAvCodecParameters()->codec_id = AV_CODEC_ID_H264;
    const uint8_t avcC[] = {0x01, 0x64, 0x00, 0x33, 0xFF, 0xE0, 0x00};
    context->setExtradata(avcC, sizeof(avcC));

    // One 300-byte NAL: its 4-byte size prefix 00 00 01 2C reads as a start code.
    constexpr uint32_t kNalSize = 300;
    std::vector<uint8_t> data(4 + kNalSize, 0x41);
    qToBigEndian<uint32_t>(kNalSize, data.data());

    auto packet = std::make_shared<QnWritableCompressedVideoData>(data.size(), context);
    packet->compressionType = AV_CODEC_ID_H264;
    packet->m_data.write((const char*) data.data(), data.size());

    DataChannelVideo dataChannelVideo;
    dataChannelVideo.setDataPacket(packet);
    nx::Buffer message;
    ASSERT_TRUE(dataChannelVideo.getNextPacket(message));
    const std::string accessUnit = message.toStdString().substr(DataChannelVideo::kHeaderSize);
    EXPECT_EQ(accessUnit.substr(0, 4), std::string("\x00\x00\x00\x01", 4));
}

TEST(DataChannelVideo, theRtpTimestampFollowsUtcAndRestartsAtARebase)
{
    using namespace std::chrono;

    const auto deviceId = nx::Uuid::createUuid();
    DataChannelVideo dataChannelVideo;
    const auto feed =
        [&](int64_t timestampUs)
        {
            auto packet = std::make_shared<QnWritableCompressedVideoData>();
            packet->compressionType = AV_CODEC_ID_H264;
            packet->deviceId = deviceId;
            packet->timestamp = timestampUs;
            dataChannelVideo.setDataPacket(packet);
            return dataChannelVideo.getLastTimestamps(deviceId);
        };

    // A UTC time in microseconds times 90 kHz is past int64.
    const auto first = feed(1761740000123456);
    EXPECT_EQ(first.ntpTimestamp, 1761740000123456);
    EXPECT_EQ((int64_t) first.rtpTimestamp, (1761740000LL * 90000 + 11111) % (1LL << 32));

    dataChannelVideo.rebaseOutputTimestamps(deviceId, 1s);
    EXPECT_EQ(feed(1761740000163456).rtpTimestamp, 90000U);
    EXPECT_EQ(feed(1761740000203456).rtpTimestamp, 90000U + 3600);
}

TEST(DataChannelVideo, anEmptyFrameStillMakesAMessage)
{
    const auto message = DataChannelVideo::makeMessage(DataChannelVideo::Frame{}, nullptr, 0);
    EXPECT_EQ(message.size(), DataChannelVideo::kHeaderSize);
}

} // namespace nx::webrtc::test
