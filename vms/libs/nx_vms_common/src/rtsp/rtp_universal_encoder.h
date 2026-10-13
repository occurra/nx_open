// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <optional>

#include <nx/rtp/rtcp.h>
#include <nx/rtp/rtcp_nack_responder.h>
#include <nx/utils/byte_array.h>
#include <transcoding/ffmpeg_transcoder.h>

#include "rtsp/abstract_rtsp_encoder.h"
#include "srtp_encryptor.h"

class NX_VMS_COMMON_API QnUniversalRtpEncoder: public AbstractRtspEncoder
{
public:
    struct Config
    {
        Config() = default;

        bool absoluteRtcpTimestamps = false;
        bool addOnvifHeaderExtension = false;
        bool addAbsCaptureTime = false;
        // Enable the playout-delay RTP header extension (min=max=0), stamped only
        // on seek-delivered frames - those tagged MediaFlags_PreRoll or
        // MediaFlags_BOF (see setDataPacket). min=max=0 puts the receiver (Chrome)
        // into its low-latency render path so the seek frame is shown as soon as it
        // decodes instead of being held by playout smoothing. It is deliberately
        // NOT stamped on steady-state frames, which keep Chrome's jitter buffer.
        bool addPlayoutDelay = false;
        bool useMultipleSdpPayloadTypes = false;
        bool useRtcpNack = false;
        bool webRtcMode = false;
        QnFfmpegTranscoder::Config transcoderConfig;
    };

public:
    QnUniversalRtpEncoder(const Config& config, nx::metric::Storage* metrics);

    bool open(
        QnConstAbstractMediaDataPtr mediaHigh,
        QnConstAbstractMediaDataPtr mediaLow,
        MediaQuality requiredQuality,
        AVCodecID transcodeToCodec,
        const QSize& videoSize,
        const nx::core::transcoding::Settings& extraTranscodeParams);

    bool open(const AVCodecParameters* codecParameters);

    virtual QString getSdpMedia(bool isVideo, int trackId, int port = 0, bool ssl = false)
        override;

    virtual void setDataPacket(QnConstAbstractMediaDataPtr media) override;
    virtual bool getNextPacket(nx::utils::ByteArray& sendBuffer) override;
    virtual void init() override;
    virtual bool isEof() const override;
    virtual void setMtu(int mtu) override;

    void setRtcpPacket(uint8_t* data, int size);
    bool getNextNackPacket(nx::utils::ByteArray& sendBuffer);
    void setSrtpEncryptionData(const nx::rtsp::EncryptionData& data);
    nx::rtsp::SrtpEncryptor* encryptor() const;
    FfmpegMuxer::PacketTimestamp getLastTimestamps() const;
    int payloadType() const;

    /**
     * Codec the encoder was opened for. A packet of any other codec is rejected
     * with EOF, see setDataPacket().
     */
    AVCodecID sourceCodec() const { return m_sourceCodec; }

    /**
     * Clears the EOF raised by a rejected packet so the encoder resumes with the
     * next one. For a caller that removed the cause of the rejection without
     * rebuilding the encoder - e.g. by inserting a transcoder that converts the
     * new source codec into the one the encoder was opened for.
     */
    void clearEof() { m_eof = false; }

    /**
     * Restarts the RTP timeline at baseUs (see FfmpegMuxer::rebaseOutputTimestamps)
     * and forces an RTCP sender report so the receiver re-learns the NTP-RTP
     * mapping immediately instead of waiting for the next report interval.
     */
    void rebaseOutputTimestamps(std::chrono::microseconds baseUs);

    QStringList sdpAttributes() const;

    void setMid(const std::string& value) { m_mid = value; }
    std::string mid() const { return m_mid; }

    /**
     * Seeds the outgoing RTP sequence number. Once set, the encoder writes the
     * sequence field itself instead of passing the muxer's through, so an encoder
     * rebuilt on a reused ssrc can continue the sequence forward. A rebuild that
     * kept the ssrc but restarted the sequence at a fresh random base would trip
     * the receiver's SRTP replay window and blank the stream until the sequence
     * climbed back past the previous high.
     */
    void setStartSequence(uint16_t sequence) { m_nextSequence = sequence; }

    /**
     * Next sequence number the encoder will emit, or none if it was never seeded.
     * A rebuilt encoder reads this off its predecessor to continue forward.
     */
    std::optional<uint16_t> nextSequence() const { return m_nextSequence; }

private:
    QSize getTargetSize(QnConstAbstractMediaDataPtr media, QSize targetSize);
    void buildSdp(
        QnConstAbstractMediaDataPtr mediaHigh,
        QnConstAbstractMediaDataPtr mediaLow,
        bool transcoding,
        MediaQuality quality);

private:
    nx::utils::ByteArray m_outputBuffer;
    QStringList m_sdpAttributes;
    Config m_config;
    bool m_isCurrentPacketSecondaryStream = false;
    // Whether the current media packet (set by setDataPacket) is a seek-delivered
    // frame that should carry the min=max=0 playout-delay extension. Only true when
    // m_config.addPlayoutDelay is set and the packet is tagged pre-roll or BOF.
    bool m_useZeroPlayoutDelay = false;
    bool m_useSecondaryPayloadType = false;
    bool m_eof = false;
    bool m_transcodingEnabled = false;
    MediaQuality m_requiredQuality = MEDIA_Quality_None;
    int m_outputPos = 0;
    int m_packetIndex = 0;
    QnFfmpegTranscoder m_transcoder;
    AVCodecID m_sourceCodec = AV_CODEC_ID_NONE;
    bool m_isVideo = false;
    int m_payloadType = 0;
    // G.711 mulaw and alaw share the RTP packetization and differ only by payload type, so both
    // are offered up front and a mid-stream switch between them just relabels the wire packets
    // (from m_sourceCodec, in getNextPacket) instead of renegotiating. Set for passthrough G.711.
    bool m_g711DualPayload = false;
    nx::rtp::RtcpSenderReporter m_rtcpReporter;
    bool m_forceRtcpReport = false;
    std::unique_ptr<nx::rtsp::SrtpEncryptor> m_encryptor;
    std::unique_ptr<nx::rtp::RtcpNackResponder> m_rtcpNackResponder;
    std::string m_mid;
    std::optional<uint16_t> m_nextSequence;
};

using QnUniversalRtpEncoderPtr = std::unique_ptr<QnUniversalRtpEncoder>;
