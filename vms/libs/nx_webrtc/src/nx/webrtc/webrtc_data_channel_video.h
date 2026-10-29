// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>

#include <nx/media/media_data_packet.h>
#include <nx/utils/buffer.h>
#include <nx/utils/uuid.h>
#include <transcoding/ffmpeg_muxer.h>

namespace nx::webrtc {

/**
 * Video the peer's WebRTC receive path refuses. libwebrtc drops an H.264 SPS above the level 5.2
 * frame size, 36864 macroblocks or a side above 8688 pixels, before any decoder sees it, with no
 * fallback and no error. Such frames reach the player as binary data channel messages instead, one
 * access unit per message, and the player decodes them with WebCodecs. Fed and drained like an
 * RTP encoder: setDataPacket() takes a frame, getNextPacket() gives its message.
 *
 * A message is a big-endian header followed by the Annex B access unit: "OCDV", the version, the
 * codec, two reserved bytes, the media flags, the device id as RFC 4122 bytes, the frame number,
 * the RTP timestamp, the UTC time in microseconds. Messages ride unordered and without
 * retransmissions: a lost message loses its frame, and the player waits for the next key frame.
 */
class NX_WEBRTC_API DataChannelVideo
{
public:
    /** The identity of one frame, as its message carries it. */
    struct Frame
    {
        nx::Uuid deviceId;
        uint32_t index = 0;
        uint32_t rtpTimestamp = 0;
        int64_t timestampUs = 0;
        /** The packet's own flags; the player reads the key frame, BOF and pre-roll bits. */
        QnAbstractMediaData::MediaFlags flags;
    };

    static constexpr int kMaxH264MacroblocksLevel52 = 36864;
    static constexpr int kMaxH264PixelSideLevel52 = 8688;
    static constexpr char kMagic[4] = {'O', 'C', 'D', 'V'};
    static constexpr uint8_t kVersion = 1;
    static constexpr uint8_t kCodecH264 = 1;
    static constexpr size_t kHeaderSize = 44;

    /** Whether the peer's WebRTC receive path refuses media, so it has to go through here. */
    static bool isRefusedByWebRtc(const QnConstAbstractMediaDataPtr& media);

    /** The data channel message of one frame. */
    static nx::Buffer makeMessage(const Frame& frame, const uint8_t* data, size_t size);

    /** Takes a frame; its message waits for getNextPacket(). */
    void setDataPacket(const QnConstAbstractMediaDataPtr& media);

    /**
     * Moves the message of the last frame taken into message.
     * @return False once the message is taken.
     */
    bool getNextPacket(nx::Buffer& message);

    /** The UTC and RTP timestamps of the device's last frame taken; zeros before the first. */
    FfmpegMuxer::PacketTimestamp getLastTimestamps(nx::Uuid deviceId) const;

    /**
     * Restarts the device's RTP timeline at baseUs with its next frame, the way
     * QnUniversalRtpEncoder::rebaseOutputTimestamps meets the base a seek's player gates on.
     */
    void rebaseOutputTimestamps(nx::Uuid deviceId, std::chrono::microseconds baseUs);

private:
    struct Device
    {
        uint32_t frameIndex = 0;
        int64_t rtpOffset90k = 0;
        FfmpegMuxer::PacketTimestamp lastTimestamps;
        std::optional<std::chrono::microseconds> pendingBaseUs;
    };

    std::map<nx::Uuid, Device> m_devices;
    std::optional<nx::Buffer> m_message;
};

} // namespace nx::webrtc
