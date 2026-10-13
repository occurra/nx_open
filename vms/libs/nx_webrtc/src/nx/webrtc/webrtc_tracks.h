// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <nx/utils/uuid.h>

#include "webrtc_utils.h"

namespace nx::webrtc {

enum class TrackType
{
    unknown,
    video,
    audio
};

struct NX_WEBRTC_API Track
{
    uint32_t ssrc = 0;
    std::string cname;
    std::string streamId;
    std::string trackId;
    int mid = -1;
    int payloadType = 0;
    Purpose purpose = Purpose::sendrecv;
    nx::Uuid deviceId;
    TrackType trackType = TrackType::unknown;

    TrackState offerState{};
    TrackState answerState{};
    bool useTranscoding = false;
};

const std::string& toSdpAttribute(Purpose purpose);

class NX_WEBRTC_API Tracks
{
    Tracks(const Tracks&) = delete;
    Tracks& operator=(const Tracks&) = delete;

public:
    void addTrack(std::unique_ptr<Track> track);

    Tracks(Session* session);
    virtual ~Tracks() = default;

    Track* videoTrack(nx::Uuid deviceId) const;
    Track* audioTrack(nx::Uuid deviceId) const;
    Track* track(uint32_t ssrc) const;

    /// Assign a fresh SSRC to a device's track and re-key it. Used on a codec
    /// switch so the peer builds a new SRTP/jitter context for the rebuilt
    /// encoder, instead of discarding the reseeded sequence numbers under the
    /// old SSRC's replay window. Returns the new SSRC, or 0 if no such track.
    uint32_t rotateSsrc(nx::Uuid deviceId, TrackType type);

    /// Re-point every track of device `from` at device `to`, keeping its ssrc,
    /// mid and payload type. deviceId is a server-side association only - it
    /// never appears in the SDP - so an in-place camera switch keeps the
    /// negotiated m-lines valid and needs no offer/answer.
    /// @return Number of tracks re-pointed.
    int retargetDevice(nx::Uuid from, nx::Uuid to);

    /// The given devices ordered by the mid of their video track, lowest first, so a camera
    /// switch hands the freed m-lines to the arriving cameras in the order the peer expects.
    /// A device with no video track sorts last.
    std::vector<nx::Uuid> orderByMid(std::vector<nx::Uuid> deviceIds) const;

    std::vector<Track> allTracks() const;
    std::map<uint32_t, std::unique_ptr<Track>>& tracks() { return m_tracks; }

    virtual bool examineSdp(const std::string& sdp) = 0;

    virtual std::string getSdpForTrack(const Track* track, uint16_t port) const;

protected:
    Session* m_session = nullptr;

private:
    std::map<uint32_t, std::unique_ptr<Track>> m_tracks;
    int m_lastMid = 0;
};

class NX_WEBRTC_API TracksForSend: public Tracks
{
    using base_type = Tracks;

public:
    TracksForSend(Session* session);
    virtual ~TracksForSend() override = default;

    virtual bool examineSdp(const std::string& sdp) override;
    virtual std::string getSdpForTrack(const Track* track, uint16_t port) const override;
};

} // namespace nx::webrtc
