// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nx/utils/uuid.h>

#include "webrtc_consumer.h"
#include "webrtc_ice.h"

namespace nx::webrtc {

/**
 * Talk-back callbacks the media_server installs on a video (Purpose::send)
 * session so the browser's microphone can ride the same data channel as the
 * playback commands, instead of a separate connection (see Streamer).
 */
struct TalkHandlers
{
    /** Talk-back turned on or off for a set of cameras: {"talk":{"deviceIds":[...],"on"}}
     *  (a legacy single "deviceId" is accepted as a one-element list). */
    std::function<void(const std::vector<nx::Uuid>& deviceIds, bool on)> onControl;

    /** One Opus packet for a camera, carried by a deviceId-prefixed binary frame. Taken by
     *  value so the payload moves from the frame all the way to the worker with no copy. */
    std::function<void(const nx::Uuid& deviceId, std::string opus)> onAudio;
};

class NX_WEBRTC_API Streamer: public AbstractIceDelegate
{
public:
    Streamer(Session* session);

    /** Routes the {"talk"} control command and binary audio frames to the media_server. */
    void setTalkHandlers(TalkHandlers handlers);

    // From AbstractIceDelegate.
    virtual bool start(Ice* ice, Dtls* dtls) override final;
    virtual void stop() override final;
    virtual bool onSrtp(std::vector<uint8_t> buffer) override final;
    virtual void onDataChannelString(const std::string& data, int streamId) override final;
    virtual void onDataChannelBinary(const std::string& data, int streamId) override final;
    virtual ~Streamer() override;

    // Interface for Consumer.
    Ice* ice();
    /**
     * {"deviceId", "timestampUs", "rtpTimestamp", "audio": {"timestampUs", "rtpTimestamp"}} - the
     * RTP-to-UTC mapping of deviceId's video and, when it has some, of its audio.
     */
    void sendTimestamp(
        nx::Uuid deviceId,
        const FfmpegMuxer::PacketTimestamp& video,
        const std::optional<FfmpegMuxer::PacketTimestamp>& audio);
    /** {"seq": n, "positions": [{"deviceId", "timestampUs"}, ...]} - where paused playback
     *  settled per camera after the frame step the client tagged with seq. */
    void sendPosition(int seq, const std::vector<CameraPosition>& positions);
    /**
     * Sends {"deviceId", "timestampMs", "status"} - the reply every playback command gets.
     * seq, when non-zero, is echoed from the command being answered, so the client can pair
     * the reply with it; server-initiated statuses (end of archive, reconnect) carry none.
     */
    void handleStreamStatus(nx::Uuid deviceId, Consumer::StreamStatus result, int seq = 0);
    /**
     * Sends {"deviceId", "videoTransport": "dataChannel" | "rtp"}: where the camera's video
     * travels from now on, so the player reads the right source. See DataChannelVideo.
     */
    void sendVideoTransport(nx::Uuid deviceId, bool isDataChannel);

    std::string idForToStringFromPtr() const;

private:
    void updateMetrics(int value);

private:
    Session* m_session = nullptr;
    Ice* m_ice = nullptr;
    IceCandidate::Filter m_iceType = IceCandidate::Filter::None;
    TalkHandlers m_talkHandlers;
};

} // namespace nx::webrtc
