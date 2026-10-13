// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "webrtc_tracks.h"

#include <algorithm>
#include <limits>

#include <nx/utils/log/log.h>
#include <nx/utils/random.h>

#include "webrtc_session.h"

namespace nx::webrtc {

namespace {

static const std::string kSendOnlyAttr = "a=sendonly";
static const std::string kRecvOnlyAttr = "a=recvonly";
static const std::string kSendRecvAttr = "a=sendrecv";

} // namespace

const std::string& toSdpAttribute(Purpose purpose)
{
    switch (purpose)
    {
        case Purpose::send:
            return kSendOnlyAttr;
        case Purpose::recv:
            return kRecvOnlyAttr;
        case Purpose::sendrecv:
        default:
            return kSendRecvAttr;
    }
}

Track* Tracks::videoTrack(nx::Uuid deviceId) const
{
    for (const auto& [_, track]: m_tracks)
    {
        if (track->deviceId == deviceId && track->trackType == TrackType::video)
            return track.get();
    }
    return nullptr;
}
Track* Tracks::audioTrack(nx::Uuid deviceId) const
{
    for (const auto& [_, track]: m_tracks)
    {
        if (track->deviceId == deviceId && track->trackType == TrackType::audio)
            return track.get();
    }
    return nullptr;
}

int Tracks::retargetDevice(nx::Uuid from, nx::Uuid to)
{
    int count = 0;
    for (const auto& [_, track]: m_tracks)
    {
        if (track->deviceId != from)
            continue;
        track->deviceId = to;
        ++count;
    }
    return count;
}

std::vector<nx::Uuid> Tracks::orderByMid(std::vector<nx::Uuid> deviceIds) const
{
    const auto midOf = [this](const nx::Uuid& deviceId)
    {
        const auto track = videoTrack(deviceId);
        return track ? track->mid : std::numeric_limits<int>::max();
    };
    std::stable_sort(
        deviceIds.begin(),
        deviceIds.end(),
        [&midOf](const nx::Uuid& a, const nx::Uuid& b)
        {
            return midOf(a) < midOf(b);
        });
    return deviceIds;
}

std::vector<Track> Tracks::allTracks() const
{
    std::vector<Track> result;
    for (const auto& [_, track]: m_tracks)
        result.emplace_back(*track.get());

    std::ranges::sort(
        result,
        [](const auto& t1, const auto& t2)
        {
            return t1.mid < t2.mid;
        });

    return result;
}

Track* Tracks::track(uint32_t ssrc) const
{
    const auto it = m_tracks.find(ssrc);
    return it != m_tracks.end() ? it->second.get() : nullptr;
}

uint32_t Tracks::rotateSsrc(nx::Uuid deviceId, TrackType type)
{
    for (auto it = m_tracks.begin(); it != m_tracks.end(); ++it)
    {
        if (it->second->deviceId == deviceId && it->second->trackType == type)
        {
            auto track = std::move(it->second);
            m_tracks.erase(it);
            do
            {
                track->ssrc = nx::utils::random::number<uint32_t>();
            } while (m_tracks.find(track->ssrc) != m_tracks.end());
            const uint32_t newSsrc = track->ssrc;
            m_tracks.emplace(newSsrc, std::move(track));
            return newSsrc;
        }
    }
    return 0;
}

Tracks::Tracks(Session* session):
    m_session(session)
{
}

void Tracks::addTrack(std::unique_ptr<Track> track)
{
    do
    {
        track->ssrc = nx::utils::random::number<uint32_t>();
    } while (m_tracks.find(track->ssrc) != m_tracks.end());
    track->purpose = m_session->purpose();

    // Find existing track with same deviceId
    bool found = false;
    for (const auto& [_, t]: m_tracks)
    {
        if (t->deviceId == track->deviceId)
        {
            track->cname = t->cname;
            track->streamId = t->streamId;
            found = true;
            break;
        }
    }
    if (!found)
    {
        track->cname = nx::Uuid::createUuid().toSimpleStdString();
        const nx::Uuid guid = track->deviceId.isNull() ? nx::Uuid::createUuid() : track->deviceId;
        track->streamId = guid.toSimpleStdString();
    }
    track->trackId = nx::Uuid::createUuid().toSimpleStdString();

    for (auto it = m_tracks.begin(); it != m_tracks.end(); ++it)
    {
        const auto& t = it->second;
        if (t->offerState == TrackState::inactive && t->trackType == track->trackType)
        {
            track->mid = t->mid;
            m_tracks.erase(it);
            break;
        }
    }

    if (track->mid == -1)
        track->mid = m_lastMid++;
    m_tracks.emplace(track->ssrc, std::move(track));
}

bool TracksForSend::examineSdp(const std::string& /*sdp*/)
{
    return true;
}

std::string Tracks::getSdpForTrack(const Track* track, uint16_t /*port*/) const
{
    std::string sdp;
#define ENDL "\r\n";
    // Used with STUN handshake
    // Media ID.
    sdp += "a=mid:" + std::to_string(track->mid) + ENDL;
    /* https://datatracker.ietf.org/doc/html/rfc3264#section-5.1
     * If the offerer wishes to only send media on a stream to its peer, it
     * MUST mark the stream as sendonly with the "a=sendonly" attribute.
     * */
    // Probably unused fields.
    sdp += "a=msid:" + track->streamId + " " + track->trackId + ENDL;
    if (track->offerState == TrackState::active)
    {
        sdp += toSdpAttribute(track->purpose) + ENDL;
    }
    else
    {
        sdp += "a=inactive" ENDL;
    }
    // Mux rtcp and rtp stream.
    sdp += "a=rtcp-mux" ENDL;
    /* 'actpass' due to bug in old Chromium. Actually, for incoming connection, should be
     * 'passive': https://datatracker.ietf.org/doc/html/rfc4145#section-4 */
    sdp += "a=setup:actpass" ENDL;
    // 'ssrc' of track used by Chromium's demuxer. Not sure about 'cname'.
    sdp += "a=ssrc:" + std::to_string(track->ssrc) + " cname:" + track->cname + ENDL;
    // Generic RTCP feedbacks supported: https://www.rfc-editor.org/rfc/rfc4585.html#section-3.6.2
    sdp += "a=rtcp-fb:" + std::to_string(track->payloadType) + " nack" ENDL;

    // Mid in RTP extension is not used now.
    // sdp += "a=extmap:1 urn:ietf:params:rtp-hdrext:sdes:mid" ENDL;

    // Absolute capture time — lets the browser know each frame's UTC capture
    // time. Off by default (see SessionConfig::addAbsCaptureTime); the SDP
    // advertisement and the actual extension emission in the encoder are
    // gated by the same flag, so they stay in lockstep.
    if (m_session->config().addAbsCaptureTime)
        sdp += "a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-capture-time" ENDL;

    // Playout-delay (video only): min=max=0 asks Chrome to render each decoded
    // frame immediately (low-latency path) instead of holding it in the playout
    // buffer. The encoder stamps it for video tracks only, so advertise it only
    // there to keep the SDP and the emitted extension in lockstep.
    if (m_session->config().addPlayoutDelay && track->trackType == TrackType::video)
        sdp += "a=extmap:3 http://www.webrtc.org/experiments/rtp-hdrext/playout-delay" ENDL;

    return sdp;
}

std::string TracksForSend::getSdpForTrack(const Track* track, uint16_t port) const
{
    bool isVideo = track->trackType == TrackType::video;
    const auto muxer = m_session->muxer();
    auto encoder =
        isVideo ? muxer->videoEncoder(track->deviceId) : muxer->audioEncoder(track->deviceId);
    if (!encoder)
    {
        NX_DEBUG(this, "Encoder is not initialized for track %1:%2", track->deviceId, track->ssrc);
        return {};
    }
    return encoder->getSdpMedia(isVideo, track->mid, port, /*ssl*/ true).toStdString()
        + base_type::getSdpForTrack(track, port);
}

TracksForSend::TracksForSend(Session* session):
    Tracks(session)
{
}

} // namespace nx::webrtc
