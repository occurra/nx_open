// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "webrtc_transcoder.h"

#include <nx/codec/h265/hevc_decoder_configuration_record.h>
#include <nx/codec/mime.h>
#include <nx/codec/nal_extradata.h>
#include <transcoding/transcoding_utils.h>

#include "nx_webrtc_ini.h"
#include "webrtc_session.h"
#include "webrtc_session_pool.h"

namespace {

static const std::set<AVCodecID> kSrtpVideoCodecs =
    {AV_CODEC_ID_VP8, AV_CODEC_ID_VP9, AV_CODEC_ID_AV1, AV_CODEC_ID_H264, AV_CODEC_ID_H265};

static const std::set<AVCodecID> kSrtpAudioCodecs =
    {AV_CODEC_ID_PCM_MULAW, AV_CODEC_ID_PCM_ALAW, AV_CODEC_ID_ADPCM_G722, AV_CODEC_ID_OPUS};

/* For some NATs, there can be another network layer, such as VPN,
 * which fragments original UDP packets with RTP.
 * Note that RTP packets with UDP transport can't be reassembled:
 * there is no information about original RTP packet size in RTP packet via UDP.
 *
 * 1200 (the libwebrtc default) rather than 1300: tunnels commonly have a
 * 1280 MTU (IPv6 minimum - WireGuard, Tailscale), and an oversized IPv6
 * packet is dropped silently instead of fragmented, which black-holes
 * exactly the key-frame packets while STUN/DTLS/audio still pass.
 * Measured on Tailscale: 1232-byte UDP payloads pass, 1300 vanish.
 * */
static const int kWebrtcMtu = 1200;

// Step this far ahead of the last emitted sequence when rebuilding an encoder on a reused
// ssrc, so the new sequence lands clear of any packet still in flight from the old one.
static const uint16_t kSequenceRebuildGap = 100;

// Carry the RTP sequence forward from the encoder being replaced into its successor. The
// successor reuses the same ssrc; continuing the sequence forward (rather than letting the
// muxer pick a fresh random base that may fall behind) keeps it clear of the receiver's SRTP
// replay window. First-time encoders start at zero and own the counter from then on.
void seedSequenceForward(
    const std::map<nx::Uuid, QnUniversalRtpEncoderPtr>& encoders,
    const nx::Uuid& deviceId,
    QnUniversalRtpEncoder& encoder)
{
    const auto it = encoders.find(deviceId);
    if (it != encoders.end() && it->second->nextSequence())
        encoder.setStartSequence(uint16_t(*it->second->nextSequence() + kSequenceRebuildGap));
    else
        encoder.setStartSequence(0);
}

// Step this far (media time) ahead of the old encoder's last output when rebuilding on a
// reused ssrc, so the new timeline lands well clear of it. Applied equally to video and audio,
// so their relation - and A/V sync - is preserved. rebaseOutputTimestamps forces a fresh RTCP
// sender report, so the peer re-learns the NTP-RTP mapping and renders the jump immediately.
static const std::chrono::microseconds kTimestampRebuildGap{100'000'000}; //< 100 s.

// Continue the RTP timestamp forward from the encoder being replaced, the same way
// seedSequenceForward continues the sequence. A rebuilt encoder on a reused ssrc otherwise
// starts its timeline at a fresh base that may fall behind the last emitted timestamp; the
// peer then discards those "past" frames - the exact analogue of a backward sequence - and the
// picture waits for the next key frame. rtpClockHz is the stream's RTP clock (90 kHz video,
// the sample rate for audio) - the same media base rescales to each stream's own clock.
void seedTimestampForward(
    const std::map<nx::Uuid, QnUniversalRtpEncoderPtr>& encoders,
    const nx::Uuid& deviceId,
    QnUniversalRtpEncoder& encoder,
    int64_t rtpClockHz)
{
    const auto it = encoders.find(deviceId);
    if (it == encoders.end() || rtpClockHz <= 0)
        return;
    const int64_t lastUs =
        (int64_t) it->second->getLastTimestamps().rtpTimestamp * 1000000 / rtpClockHz;
    encoder.rebaseOutputTimestamps(std::chrono::microseconds(lastUs) + kTimestampRebuildGap);
}

} // namespace

namespace nx::webrtc {

Transcoder::Transcoder(Session* session):
    m_session(session)
{
}

bool Transcoder::createEncoders(
    nx::Uuid deviceId,
    AVCodecParameters* videoParameters,
    AVCodecParameters* audioParameters)
{
    return createRtpEncoders(deviceId, videoParameters, audioParameters);
}

bool Transcoder::createRtpEncoders(
    nx::Uuid deviceId,
    AVCodecParameters* videoParameters,
    AVCodecParameters* audioParameters)
{
    bool success = false;
    if (videoParameters)
    {
        if (auto track = m_session->tracks()->videoTrack(deviceId))
        {
            if (auto encoder =
                    createRtpEncoder(videoParameters, track->ssrc, track->cname, track->mid))
            {
                success = true;
                if (m_encryptionData.has_value())
                    encoder->setSrtpEncryptionData(*m_encryptionData);
                seedSequenceForward(m_videoEncoders, deviceId, *encoder);
                seedTimestampForward(m_videoEncoders, deviceId, *encoder, /*rtpClockHz*/ 90000);
                track->offerState = TrackState::active;
                track->payloadType = encoder->payloadType();
                m_videoEncoders[deviceId] = std::move(encoder);
            }
            else
            {
                NX_DEBUG(
                    this,
                    "Can't create Video encoder for track %1, codec: %2, device: %3",
                    track->ssrc,
                    videoParameters->codec_id,
                    track->deviceId);
            }
        }
    }

    if (audioParameters)
    {
        if (auto track = m_session->tracks()->audioTrack(deviceId))
        {
            if (auto encoder =
                    createRtpEncoder(audioParameters, track->ssrc, track->cname, track->mid))
            {
                success = true;
                if (m_encryptionData.has_value())
                    encoder->setSrtpEncryptionData(*m_encryptionData);
                seedSequenceForward(m_audioEncoders, deviceId, *encoder);
                seedTimestampForward(
                    m_audioEncoders,
                    deviceId,
                    *encoder,
                    /*rtpClockHz*/ audioParameters->sample_rate);
                track->offerState = TrackState::active;
                track->payloadType = encoder->payloadType();
                m_audioEncoders[deviceId] = std::move(encoder);
            }
            else
            {
                NX_DEBUG(
                    this,
                    "Can't create Audio encoder for track %1, codec: %2, device: %3",
                    track->ssrc,
                    audioParameters->codec_id,
                    track->deviceId);
            }
        }
    }

    return success;
}

bool Transcoder::isVideoCodecSupported(AVCodecID codecId)
{
    if (kSrtpVideoCodecs.count(codecId) == 0)
        return false;
    if (codecId == AV_CODEC_ID_H265 && !::ini().webrtcSrtpTransportEnableH265)
        return false;
    return true;
}

bool Transcoder::isAudioCodecSupported(AVCodecID codecId)
{
    return kSrtpAudioCodecs.count(codecId) != 0;
}

QnUniversalRtpEncoderPtr Transcoder::createRtpEncoder(
    AVCodecParameters* codecParameters,
    uint32_t ssrc,
    const std::string& cname,
    int /*mid*/)
{
    QnUniversalRtpEncoder::Config config;
    config.absoluteRtcpTimestamps = true;
    config.addOnvifHeaderExtension = false;
    config.addAbsCaptureTime = m_session->config().addAbsCaptureTime;
    // playout-delay is a video-only extension - stamp it only on the video track.
    config.addPlayoutDelay =
        m_session->config().addPlayoutDelay && codecParameters->codec_type == AVMEDIA_TYPE_VIDEO;
    config.useMultipleSdpPayloadTypes = false;
    config.useRtcpNack = true;
    config.webRtcMode = ::ini().webrtcFixH264ProfileConstraints;

    QnUniversalRtpEncoderPtr universalEncoder(new QnUniversalRtpEncoder(config, nullptr));
    universalEncoder->setSsrc(ssrc);
    universalEncoder->setCName(cname);
    universalEncoder->setMtu(kWebrtcMtu);

    if (!universalEncoder->open(codecParameters))
    {
        NX_DEBUG(
            this,
            "%1: No RTSP encoder for codec %2 skip track",
            m_session->id(),
            codecParameters->codec_id);
        return nullptr;
    }

    return universalEncoder;
}

void Transcoder::setSrtpEncryptionData(const rtsp::EncryptionData& data)
{
    m_encryptionData = data;
    m_decryptor = std::make_unique<rtsp::SrtpDecryptor>();
    if (!m_decryptor->init(data.client))
        m_decryptor.reset();
    for (auto& [_, encoder]: m_videoEncoders)
        encoder->setSrtpEncryptionData(data);
    for (auto& [_, encoder]: m_audioEncoders)
        encoder->setSrtpEncryptionData(data);
}

rtsp::SrtpDecryptor* Transcoder::getDecryptor() const
{
    return m_decryptor.get();
}

QnUniversalRtpEncoder* Transcoder::videoEncoder(nx::Uuid deviceId) const
{
    auto it = m_videoEncoders.find(deviceId);
    return it != m_videoEncoders.end() ? it->second.get() : nullptr;
}

QnUniversalRtpEncoder* Transcoder::audioEncoder(nx::Uuid deviceId) const
{
    auto it = m_audioEncoders.find(deviceId);
    return it != m_audioEncoders.end() ? it->second.get() : nullptr;
}

void Transcoder::retargetDevice(nx::Uuid from, nx::Uuid to)
{
    const auto moveEntry = [from, to](auto& map)
    {
        auto it = map.find(from);
        if (it == map.end())
            return;
        auto value = std::move(it->second);
        map.erase(it);
        map.insert_or_assign(to, std::move(value));
    };

    //< The encoders keep their ssrc, sequence numbers and RTP timeline, so the
    //< peer sees one continuous stream across the switch. A new camera whose
    //< codec differs is rejected by its first packet (isEof) exactly as a
    //< mid-stream codec change is, and takes the renegotiation path.
    moveEntry(m_videoEncoders);
    moveEntry(m_audioEncoders);
    //< The RTP rebase armed for the switch was addressed to the new device, so
    //< only carry over an unconsumed one from the old device if the new device
    //< has none of its own.
    if (!m_pendingRtpRebase.contains(to))
        moveEntry(m_pendingRtpRebase);
    else
        m_pendingRtpRebase.erase(from);
    moveEntry(m_rtpBaseLinearizer);
}

QnUniversalRtpEncoder* Transcoder::encoderBySsrc(uint32_t ssrc) const
{
    if (auto track = m_session->tracks()->track(ssrc))
    {
        if (track->trackType == TrackType::video)
            return videoEncoder(track->deviceId);
        else if (track->trackType == TrackType::audio)
            return audioEncoder(track->deviceId);
    }
    return nullptr;
}

QnUniversalRtpEncoder* Transcoder::getRtpEncoder(
    nx::Uuid deviceId,
    QnAbstractMediaData::DataType type) const
{
    if (type == QnAbstractMediaData::VIDEO)
        return videoEncoder(deviceId);
    if (type == QnAbstractMediaData::AUDIO)
        return audioEncoder(deviceId);

    return nullptr;
}

QnUniversalRtpEncoder* Transcoder::setDataPacket(
    nx::Uuid deviceId,
    QnConstAbstractMediaDataPtr media)
{
    m_lastTimestampMs = media->timestamp / 1000;

    const auto rebase = m_pendingRtpRebase.find(deviceId);
    const bool rebaseMatches = rebase != m_pendingRtpRebase.end()
        && (!rebase->second.seekSeq || media->opaque == *rebase->second.seekSeq);
    if (rebaseMatches)
    {
        // Round up so the first frame lands at or past the base the player
        // gates on. One shared microsecond base keeps video and audio aligned:
        // each muxer rescales it to its own stream clock.
        NX_VERBOSE(
            this,
            "SEEKPROF first post-seek packet seq=%1 mediaTs=%2ms",
            media->opaque,
            media->timestamp / 1000);
        const std::chrono::microseconds baseUs((rebase->second.rtpBase90k * 100 + 8) / 9);
        if (auto encoder = videoEncoder(deviceId))
            encoder->rebaseOutputTimestamps(baseUs);
        if (auto encoder = audioEncoder(deviceId))
            encoder->rebaseOutputTimestamps(baseUs);
        m_dataChannelVideo.rebaseOutputTimestamps(deviceId, baseUs);
    }

    QnUniversalRtpEncoder* rtpEncoder = nullptr;
    bool isTaken = false;
    if (DataChannelVideo::isRefusedByWebRtc(media))
    {
        m_dataChannelVideo.setDataPacket(media);
        isTaken = true;
    }
    else
    {
        rtpEncoder = getRtpEncoder(deviceId, media->dataType);
        if (!rtpEncoder)
            return nullptr; //< Skip not supported data.

        rtpEncoder->setDataPacket(media);
        // A codec this encoder cannot accept raises EOF instead of consuming the
        // packet: the Consumer holds the packet, the session rebuilds the encoder
        // against the new codec and re-feeds it, and it is THAT encoder - brand
        // new, its output timeline starting at zero - that needs the base applied.
        // Spending it here regardless left a camera switch which changes codec
        // streaming from zero while the player waited for the base it asked for,
        // so it stayed frozen on the previous camera until the freeze timed out.
        isTaken = !rtpEncoder->isEof();
    }

    // The rebase is spent only once a packet is really taken.
    if (rebaseMatches && isTaken)
        m_pendingRtpRebase.erase(rebase);

    return rtpEncoder;
}

void Transcoder::setPendingRtpRebase(
    nx::Uuid deviceId,
    std::optional<int> seekSeq,
    int64_t rtpBase90k)
{
    // rtpBase90k arrives as a 32-bit RTP timestamp (the player's base % 2^32).
    // Linearize it into a monotonic 64-bit value so a wrap past 2^32 mid-scrub
    // does not drive the muxer's output DTS backwards (which would make
    // FfmpegMuxer reject every packet and freeze the stream). Start the timeline
    // at 0 (the base only ever advances, so it never wraps below zero) - the
    // default start is near int64 max and would overflow the *100 rescale in
    // setDataPacket().
    auto& linearizer = m_rtpBaseLinearizer.try_emplace(deviceId, /*startValue*/ 0).first->second;
    m_pendingRtpRebase[deviceId] = {seekSeq, linearizer.linearize((uint32_t) rtpBase90k)};
}

bool Transcoder::getNextPacket(QnUniversalRtpEncoder* rtpEncoder, nx::utils::ByteArray& buffer)
{
    return rtpEncoder->getNextPacket(buffer);
}

bool Transcoder::isEof(QnUniversalRtpEncoder* rtpEncoder) const
{
    return rtpEncoder->isEof();
}

} // namespace nx::webrtc
