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
 * the RTP timestamp, the UTC time in microseconds, the frame width and height. Messages ride
 * unordered and are retransmitted only while fresh: the player puts late frames back in order,
 * and a frame lost for good makes it wait for the next key frame.
 */
class NX_WEBRTC_API DataChannelVideo
{
public:
    /** The identity of one frame, as its message carries it. */
    struct Frame
    {
        /** The device the frame belongs to. */
        nx::Uuid deviceId;

        /** The number of the frame in its device's stream, from 0. */
        uint32_t index = 0;

        /** The time of the frame on its device's 90 kHz timeline, which sync points map to UTC. */
        uint32_t rtpTimestamp = 0;

        /** The UTC time of the frame. */
        int64_t timestampUs = 0;

        /** The packet's own flags; the player reads the key frame and pre-roll bits. */
        QnAbstractMediaData::MediaFlags flags;

        /**
         * The frame width of the codec parameters, 0 when unknown. The player configures its
         * decoder with the size: without it, Chrome picks a hardware decoder, which fails on a
         * frame wider than it takes.
         */
        uint16_t width = 0;

        /** The frame height of the codec parameters, 0 when unknown. */
        uint16_t height = 0;
    };

    /** The largest frame of H.264 level 5.2 in macroblocks: 4096x2304. */
    static constexpr int kMaxH264MacroblocksLevel52 = 36864;

    /** The longest frame side of H.264 level 5.2: sqrt(8 * 36864) macroblocks. */
    static constexpr int kMaxH264PixelSideLevel52 = 8688;

    /** The first bytes of a message, which tell it from the other binary messages. */
    static constexpr char kMagic[4] = {'O', 'C', 'D', 'V'};

    /** The version of the message layout. */
    static constexpr uint8_t kVersion = 2;

    /** The codec byte of an H.264 frame. */
    static constexpr uint8_t kCodecH264 = 1;

    /** The size of the header in bytes; the access unit follows it. */
    static constexpr size_t kHeaderSize = 48;

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
    /** The timeline of one device. */
    struct Device
    {
        /** The index of the device's next frame. */
        uint32_t frameIndex = 0;

        /** What the last rebase adds to the 90 kHz clock of a frame's UTC time. */
        int64_t rtpOffset90k = 0;

        /** The timestamps of the device's last frame, for its sync points. */
        FfmpegMuxer::PacketTimestamp lastTimestamps;

        /** The base of a rebase that waits for the device's next frame. */
        std::optional<std::chrono::microseconds> pendingBaseUs;
    };

    /** The timeline of each device that sent a frame or got a rebase. */
    std::map<nx::Uuid, Device> m_devices;

    /** The message of the last frame taken, until getNextPacket() moves it out. */
    std::optional<nx::Buffer> m_message;
};

} // namespace nx::webrtc
