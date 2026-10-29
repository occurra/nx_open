// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "webrtc_data_channel_video.h"

#include <nx/codec/h264/sequence_parameter_set.h>
#include <nx/codec/nal_units.h>
#include <nx/media/h264_utils.h>
#include <nx/media/video_data_packet.h>
#include <nx/utils/bit_stream.h>
#include <nx/utils/log/log.h>

namespace nx::webrtc {

namespace {

constexpr int64_t kRtpClockHz = 90000;
constexpr int64_t kUsPerSecond = 1000000;
constexpr uint32_t kByteBits = 8;
constexpr uint32_t kWordBits = 32;

int64_t toRtp90k(int64_t timestampUs)
{
    return timestampUs * kRtpClockHz / kUsPerSecond;
}

void writeHeader(uint8_t* header, const DataChannelVideo::Frame& frame)
{
    nx::utils::BitStreamWriter writer(header, (int) DataChannelVideo::kHeaderSize);
    writer.putBytes((const uint8_t*) DataChannelVideo::kMagic, sizeof(DataChannelVideo::kMagic));
    writer.putBits(kByteBits, DataChannelVideo::kVersion);
    writer.putBits(kByteBits, DataChannelVideo::kCodecH264);
    writer.putBits(2 * kByteBits, 0); //< Reserved.
    writer.putBits(kWordBits, (uint32_t) frame.flags);
    const QByteArray deviceId = frame.deviceId.toRfc4122();
    writer.putBytes((const uint8_t*) deviceId.data(), (uint32_t) deviceId.size());
    writer.putBits(kWordBits, frame.index);
    writer.putBits(kWordBits, frame.rtpTimestamp);
    writer.putBits(kWordBits, (uint32_t) ((uint64_t) frame.timestampUs >> kWordBits));
    writer.putBits(kWordBits, (uint32_t) frame.timestampUs);
    writer.flushBits();
}

bool exceedsLevel52(int width, int height)
{
    if (width <= 0 || height <= 0)
        return false;
    if (width > DataChannelVideo::kMaxH264PixelSideLevel52
        || height > DataChannelVideo::kMaxH264PixelSideLevel52)
    {
        return true;
    }
    const int64_t macroblocks = (int64_t) ((width + 15) / 16) * ((height + 15) / 16);
    return macroblocks > DataChannelVideo::kMaxH264MacroblocksLevel52;
}

} // namespace

bool DataChannelVideo::isRefusedByWebRtc(const QnConstAbstractMediaDataPtr& media)
{
    if (media->dataType != QnAbstractMediaData::VIDEO
        || media->compressionType != AV_CODEC_ID_H264)
    {
        return false;
    }
    const auto& context = media->getContext();
    if (context && exceedsLevel52(context->getWidth(), context->getHeight()))
        return true;
    if (context && context->getWidth() > 0)
        return false;

    // A stream whose parameters carry no size says it in the SPS of its key frames.
    if (!media->flags.testFlag(QnAbstractMediaData::MediaFlags_AVKey))
        return false;
    const auto video = std::dynamic_pointer_cast<const QnCompressedVideoData>(media);
    nx::media::h264::SequenceParameterSet sps;
    return video && nx::media::h264::extractSps(video.get(), sps)
        && exceedsLevel52(sps.getWidth(), sps.getHeight());
}

nx::Buffer DataChannelVideo::makeMessage(const Frame& frame, const uint8_t* data, size_t size)
{
    uint8_t header[kHeaderSize];
    writeHeader(header, frame);
    nx::Buffer message;
    message.reserve(kHeaderSize + size);
    message.append((const char*) header, kHeaderSize);
    message.append((const char*) data, size);
    return message;
}

void DataChannelVideo::setDataPacket(const QnConstAbstractMediaDataPtr& media)
{
    auto& device = m_devices[media->deviceId];
    if (device.pendingBaseUs)
    {
        device.rtpOffset90k =
            toRtp90k(device.pendingBaseUs->count()) - toRtp90k(media->timestamp);
        device.pendingBaseUs.reset();
    }

    Frame frame;
    frame.deviceId = media->deviceId;
    frame.index = device.frameIndex++;
    frame.rtpTimestamp = (uint32_t) (toRtp90k(media->timestamp) + device.rtpOffset90k);
    frame.timestampUs = media->timestamp;
    frame.flags = media->flags;
    device.lastRtpTimestamp = frame.rtpTimestamp;

    const size_t size = media->dataSize();
    m_message = makeMessage(frame, (const uint8_t*) media->data(), size);

    // WebCodecs takes Annex B, and the NAL units of a camera come size-prefixed as often as with
    // start codes.
    uint8_t* accessUnit = (uint8_t*) m_message->data() + kHeaderSize;
    if (size > 0 && nx::media::nal::isStartCode(accessUnit, size) == 0)
        nx::media::nal::convertToStartCodes(accessUnit, size);

    NX_VERBOSE(
        this,
        "Device %1: frame %2 (%3 bytes, key %4) goes over the data channel, rtp %5",
        media->deviceId,
        frame.index,
        size,
        frame.flags.testFlag(QnAbstractMediaData::MediaFlags_AVKey),
        frame.rtpTimestamp);
}

bool DataChannelVideo::getNextPacket(nx::Buffer& message)
{
    if (!m_message)
        return false;
    message = std::move(*m_message);
    m_message.reset();
    return true;
}

uint32_t DataChannelVideo::lastRtpTimestamp(nx::Uuid deviceId) const
{
    const auto device = m_devices.find(deviceId);
    return device == m_devices.end() ? 0 : device->second.lastRtpTimestamp;
}

void DataChannelVideo::rebaseOutputTimestamps(
    nx::Uuid deviceId,
    std::chrono::microseconds baseUs)
{
    m_devices[deviceId].pendingBaseUs = baseUs;
}

} // namespace nx::webrtc
