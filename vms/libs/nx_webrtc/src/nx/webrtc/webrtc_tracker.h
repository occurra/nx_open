// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <nx/network/websocket/websocket.h>
#include <rapidjson/document.h>

#include "webrtc_session.h"

class QnMediaServerModule;

namespace nx::webrtc {

class NX_WEBRTC_API Tracker
{
public:
    /// @param aioThread Thread to run the signalling websocket on - the
    ///     thread the socket handed to start() already lives on, so that
    ///     socket never has to be moved. Null lets the pollable pick one, for
    ///     callers that have no socket yet.
    Tracker(Session* session, nx::network::aio::AbstractAioThread* aioThread = nullptr);
    ~Tracker();

    /// @param wsRole The signaling websocket's framing role: server when the
    ///     socket was accepted (browser/relay dials us), client when this end
    ///     dialed out (media_server toward the toolbox). Either way this end
    ///     is the offerer and sends the offer right here.
    void start(
        std::unique_ptr<AbstractStreamSocket>&& socket,
        const nx::network::http::HttpHeaders& headers,
        nx::network::websocket::Role wsRole = nx::network::websocket::Role::server);
    void onIceCandidate(const IceCandidate& candidate);

    void sendOffer();
    bool isStarted() const { return m_started; }

    /// The AIO thread this session runs on. The tracker owns the signaling
    /// websocket, which outlives every ice, so its thread is the one stable
    /// choice for the session: the Consumer and the session-owned ices are
    /// bound to it at construction and never migrate.
    nx::network::aio::AbstractAioThread* getAioThread() const { return m_pollable.getAioThread(); }

    /// Rebuild the device's encoder for its new codec and send a fresh offer.
    /// Thread-safe: hops onto the tracker's aio thread before touching the
    /// websocket. Called by the Consumer when it detects a mid-stream codec
    /// change.
    void renegotiateCodec(nx::Uuid deviceId);

private:
    void readMessage();
    bool processMessages();
    AnswerResult examineAnswer();
    void sendIce(const IceCandidate& candidate);
    void flushIceCandidatesOnce();
    /** Names every camera the session currently refuses, one message each; repeating one is
     *  harmless. The session plays on for the cameras it can serve. */
    void sendUnsupportedCodecs();
    void sendJson(const rapidjson::Document& doc);
    void onBytesRead(SystemError::ErrorCode errorCode, std::size_t /*bytesTransferred*/);
    void stopUnsafe();

private:
    WebSocketPtr m_webSocket;
    Session* m_session = nullptr;
    nx::Buffer m_buffer;
    nx::network::aio::BasicPollable m_pollable;
    IceCandidate::Filter m_candidateFilter = IceCandidate::Filter::All;
    std::atomic<bool> m_started{};
    bool m_iceSent = false;
};

} // namespace nx::webrtc
