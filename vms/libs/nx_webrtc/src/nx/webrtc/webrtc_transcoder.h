// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <map>
#include <memory>
#include <optional>

#include <nx/rtp/time_linearizer.h>
#include <nx/utils/scope_guard.h>
#include <rtsp/rtp_universal_encoder.h>
#include <rtsp/rtsp_ffmpeg_encoder.h>

#include "webrtc_utils.h"

class QnMediaServerModule;

namespace nx::webrtc {

class Session;

/**
 * SRTP-only transcoder. The MSE (data-channel-muxed) playback path was
 * removed; this class now only manages per-device SRTP RTP encoders for the
 * outgoing video and audio streams.
 */
class NX_WEBRTC_API Transcoder
{
public:
    explicit Transcoder(Session* session);

    Transcoder(const Transcoder&) = delete;
    Transcoder& operator=(const Transcoder&) = delete;
    Transcoder(Transcoder&&) = delete;
    Transcoder& operator=(Transcoder&&) = delete;

    bool createEncoders(
        nx::Uuid deviceId,
        AVCodecParameters* videoParameters,
        AVCodecParameters* audioParameters);
    void setSrtpEncryptionData(const rtsp::EncryptionData& data);
    rtsp::SrtpDecryptor* getDecryptor() const;
    bool isVideoCodecSupported(AVCodecID codecId);
    bool isAudioCodecSupported(AVCodecID codecId);

    QnUniversalRtpEncoder* videoEncoder(nx::Uuid deviceId) const;
    QnUniversalRtpEncoder* audioEncoder(nx::Uuid deviceId) const;
    QnUniversalRtpEncoder* encoderBySsrc(uint32_t ssrc) const;

    /**
     * Re-key every per-device encoder and RTP-timeline entry from one device to
     * another, so an in-place camera switch keeps the encoders it already
     * negotiated - same ssrc, sequence numbers and output timeline. A new
     * camera with a different codec is rejected by its first packet exactly
     * like a mid-stream codec change and takes the renegotiation path.
     */
    void retargetDevice(nx::Uuid from, nx::Uuid to);

    // API for Consumer.
    QnUniversalRtpEncoder* setDataPacket(nx::Uuid deviceId, QnConstAbstractMediaDataPtr media);
    bool getNextPacket(QnUniversalRtpEncoder* rtpEncoder, nx::utils::ByteArray& buffer);
    bool isEof(QnUniversalRtpEncoder* rtpEncoder) const;
    FfmpegMuxer::PacketTimestamp getLastTimestamps(nx::Uuid deviceId) const;
    int64_t getLastTimestampMs() const { return m_lastTimestampMs; } //< Required for reconnection.

    /**
     * Arms an RTP timeline rebase for deviceId: when the first packet whose
     * opaque field equals seekSeq reaches setDataPacket() (the archive reader
     * marks every post-seek packet with the seek's marker), both the video and
     * the audio encoder restart their output timeline at rtpBase90k units
     * (90 kHz). The deliberate gap lets the player discard the pre-seek flush
     * by comparing frame RTP timestamps against the base it chose itself.
     *
     * Without seekSeq the next packet starts the base, whatever its marker: a live switch stops
     * the old provider, so there is no flush to exclude. The rebase waits until an encoder takes
     * a packet, so an encoder rebuilt for a codec change gets the same base.
     */
    void setPendingRtpRebase(nx::Uuid deviceId, std::optional<int> seekSeq, int64_t rtpBase90k);

private:
    QnUniversalRtpEncoderPtr createRtpEncoder(
        AVCodecParameters* codecParameters,
        uint32_t ssrc,
        const std::string& cname,
        int mid);

    bool createRtpEncoders(
        nx::Uuid deviceId,
        AVCodecParameters* videoParameters,
        AVCodecParameters* audioParameters);
    QnUniversalRtpEncoder* getRtpEncoder(nx::Uuid deviceId, QnAbstractMediaData::DataType type)
        const;

private:
    struct PendingRtpRebase
    {
        std::optional<int> seekSeq; //< Marker of the packet that starts the base; none: the next.
        int64_t rtpBase90k =
            0; //< Linearized to a monotonic 64-bit timeline (see m_rtpBaseLinearizer).
    };

private:
    Session* m_session;
    std::map<nx::Uuid, QnUniversalRtpEncoderPtr> m_videoEncoders;
    std::map<nx::Uuid, QnUniversalRtpEncoderPtr> m_audioEncoders;
    std::map<nx::Uuid, PendingRtpRebase> m_pendingRtpRebase;
    // Unwraps the player's 32-bit RTP rebase (base % 2^32) into a monotonic
    // 64-bit timeline per device. Over a long scrub the wire value wraps past
    // 2^32 and, fed straight to the muxer, drives the output DTS backwards -
    // FfmpegMuxer then rejects every packet ("non monotonically increasing dts")
    // and the stream freezes. The wire RTP still wraps; the browser unwraps it
    // for its own reveal gate, so only the muxer's internal DTS changes.
    std::map<nx::Uuid, nx::rtp::TimeLinearizer<uint32_t>> m_rtpBaseLinearizer;
    int64_t m_lastTimestampMs = 0;
    std::optional<rtsp::EncryptionData> m_encryptionData;
    std::unique_ptr<rtsp::SrtpDecryptor> m_decryptor;
};

} // namespace nx::webrtc
