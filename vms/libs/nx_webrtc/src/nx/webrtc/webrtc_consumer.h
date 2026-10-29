// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <atomic>
#include <map>
#include <optional>

#include <nx/media/annexb_to_mp4.h>
#include <nx/network/aio/basic_pollable.h>
#include <nx/network/aio/timer.h>
#include <nx/network/http/http_types.h>
#include <nx/rtp/rtcp.h>
#include <nx/rtp/rtp.h>
#include <nx/vms/api/types/stream_index.h>

#include "abstract_camera_data_provider.h"
#include "camera_position.h"
#include "media_queue.h"
#include "webrtc_data_channel_video.h"

namespace nx::webrtc {

class Session;
class Streamer;

using namespace nx::network;

class NX_WEBRTC_API Consumer
{
public:
    enum StreamStatus : int
    {
        wait = http::StatusCode::_continue, //< Don't know result at this moment.
        success = http::StatusCode::ok, //< Success.
        reconnect = http::StatusCode::movedPermanently, //< Only a reconnect can resume the stream.
        nodata = http::StatusCode::notFound, //< No provider for this timestamp, can't seek.
        endOfArchive = http::StatusCode::gone, //< The device's archive ended mid-session.
    };
    using StatusHandler = std::function<void(nx::Uuid deviceId, StreamStatus)>;

    /**
     * Client-chosen RTP-timeline rebase for a seek (VMS-166). When a seek
     * carries it, the first post-seek packet tagged with seq restarts the
     * outgoing RTP clock at rtpBase90k (90 kHz units, 64-bit virtual timeline),
     * so the player reveals post-seek frames by comparing their RTP against a
     * base it picked itself - no ack round-trip. Omitted by legacy clients: the
     * seek then keeps the continuous RTP timeline with no rebase.
     */
    struct RtpRebase
    {
        int seq = 0;
        int64_t rtpBase90k = 0;
    };

public:
    Consumer(Session* session);
    virtual ~Consumer();

    void putData(const QnConstAbstractMediaDataPtr& data);

    // API for Streamer.
    bool startStream(Streamer* streamer);
    bool handleSrtp(std::vector<uint8_t> buffer);
    /**
     * @param rebase Client-chosen RTP-timeline rebase; nullopt for legacy
     *     clients that do not send seq / rtpBase, which keeps the continuous
     *     RTP timeline with no rebase. See RtpRebase.
     */
    void seek(
        nx::Uuid deviceId,
        int64_t timestampUs,
        bool keyframeOnly,
        StatusHandler handler,
        std::optional<RtpRebase> rebase = std::nullopt);
    void startProvidersIfNeeded();
    void changeQuality(nx::Uuid deviceId, nx::vms::api::StreamIndex stream, StatusHandler handler);
    void pause(nx::Uuid deviceId, StatusHandler handler);
    /**
     * @param positionUs Optional archive position to reposition to before resuming, so a
     *     stream paused on live resumes as archive exactly at the client's held frame.
     * @param rebase Client-chosen RTP-timeline rebase for that repositioning; see seek().
     */
    void resume(
        nx::Uuid deviceId,
        std::optional<int64_t> positionUs,
        StatusHandler handler,
        std::optional<RtpRebase> rebase = std::nullopt);
    /**
     * @param seq Client-chosen sequence of the step command, echoed by the position
     *     report the provider sends when the step has settled (see reportPosition).
     * @param positionUs Optional archive position to seek to before stepping, so the step
     *     lands relative to the client's shown frame rather than the server's last
     *     delivered one (e.g. the first step out of a live pause).
     */
    void nextFrame(
        nx::Uuid deviceId,
        int seq,
        std::optional<int64_t> positionUs,
        StatusHandler handler);
    void previousFrame(
        nx::Uuid deviceId,
        int seq,
        std::optional<int64_t> positionUs,
        StatusHandler handler);
    void onDataSent(bool success);

    // API for providers.
    /** Tells the client where paused playback settled after a frame step, per camera: the
     *  position is authoritative while paused, and the client both moves its playhead and
     *  reveals each tile on its own settled frame. */
    void reportPosition(const std::vector<CameraPosition>& positions);

    /**
     * Runs func on the muxing thread, inline when already there. Every control call of the
     * providers runs on this thread, so a provider posts here whatever must join them.
     */
    void runInMuxingThread(std::function<void()>&& func);

    /// Resume media draining after a codec-change SDP renegotiation has been
    /// answered by the peer. No-op unless a renegotiation is in flight. Called
    /// by the Tracker on a successful answer.
    void resumeAfterRenegotiation();
    void setSendTimestampInterval(std::chrono::milliseconds interval);
    /**
     * Session-wide overlay metadata subscription. Clears every per-device
     * override, so the whole session follows the new default.
     */
    void setEnableMetadata(bool enableMetadata);

    /** Session default, ignoring per-device overrides. Seeds new providers. */
    bool isMetadataEnabled() const { return m_enableMetadata; }

    /**
     * Per-device overlay metadata subscription for multi-camera sessions. The
     * device keeps following its override until the next session-wide call.
     */
    void setEnableMetadata(nx::Uuid deviceId, bool enableMetadata);

    /** Effective subscription for deviceId: its override, else the default. */
    bool isMetadataEnabledFor(nx::Uuid deviceId) const;

    bool canAcceptData() const;
    std::string idForToStringFromPtr() const;

private:
    friend class Ice;

    // Streaming related.
    void processNextData();
    /// Hold the new-codec packet, pause draining and ask the session to rebuild
    /// the encoder and send a new SDP offer. Runs on the muxing thread.
    void requestCodecRenegotiation(const QnConstAbstractMediaDataPtr& media);
    void sendMetadata(const QnConstCompressedMetadataPtr& metadata);
    /** Routes a video packet the peer's WebRTC path refuses; see DataChannelVideo. */
    void sendVideoOverDataChannel(const QnConstAbstractMediaDataPtr& media);
    /** Tells the player once per change where deviceId's video travels. */
    void noteVideoTransport(nx::Uuid deviceId, bool isDataChannel);
    /**
     * Sends the RTP-to-UTC sync point of media, throttled per device; a seek's first packet and
     * every pre-roll frame report at once, so the player can reveal exactly at the target.
     */
    void sendSyncPointIfDue(
        const QnConstAbstractMediaDataPtr& media,
        int64_t ntpTimestampUs,
        uint32_t rtpTimestamp);
    void handleRtcp(uint8_t* data, int size);
    void stopUnsafe();
    void sendMediaBuffer();

private:
    // Main.
    nx::vms::server::MediaQueue m_mediaQueue;
    bool m_needStop = false;
    nx::network::aio::BasicPollable m_pollable;
    Session* m_session = nullptr;
    Streamer* m_streamer = nullptr;
    /** Per device: the cameras of a session live on independent timelines, and one
     *  camera's report must not throttle another's. */
    std::map<nx::Uuid, int64_t> m_lastReportedTimestampUs;
    /** Sequence of the frame step in flight, echoed by its position report; 0 = none. */
    int m_stepSeq = 0;
    /** Per device, whether its last video packet went over the data channel. */
    std::map<nx::Uuid, bool> m_videoOverDataChannel;
    std::chrono::milliseconds m_sendTimestampInterval{1000};
    /// Session default subscription. Atomic: written synchronously by setEnableMetadata (so a
    /// createProvider seeding a new provider right after sees it), read on both the session and
    /// the muxing thread.
    std::atomic<bool> m_enableMetadata = false;

    /**
     * Per-device overrides on top of m_enableMetadata (multi-camera sessions).
     * Mutated and read on the muxing thread only; see setEnableMetadata.
     */
    std::map<nx::Uuid, bool> m_metadataOverrides;

    bool m_sendInProgress = false;
    bool m_renegotiating = false; ///< True while a codec-change SDP
                                  ///< renegotiation is in flight. Media draining
                                  ///< is paused until the peer answers the new
                                  ///< offer, so nothing is sent under the stale
                                  ///< codec. The packet that triggered the switch
                                  ///< is pushed back to the front of m_mediaQueue
                                  ///< and re-fed to the rebuilt encoder on resume.
    nx::network::aio::Timer m_renegotiationTimer; ///< Safety net: falls back to
                                                  ///< a full reconnect if the peer never answers
                                                  ///< the renegotiation offer.
    const std::string m_sessionId;
};

} // namespace nx::webrtc
