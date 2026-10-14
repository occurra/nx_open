// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "webrtc_tracker.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <network/tcp_connection_priv.h>
#include <nx/network/http/custom_headers.h>
#include <nx/network/websocket/websocket_handshake.h>

#include "nx_webrtc_ini.h"
#include "webrtc_session_pool.h"

namespace nx::webrtc {

using namespace nx::vms::api;
using namespace nx::network;
using namespace nx;

Tracker::Tracker(Session* session, nx::network::aio::AbstractAioThread* aioThread):
    m_session(session),
    m_pollable(aioThread),
    m_sessionId(session->id())
{
    const std::string iceCandidatesIni = ::ini().webrtcIceCandidates;
    if (!iceCandidatesIni.empty())
        m_candidateFilter = IceCandidate::parseFilter(iceCandidatesIni);
}

Tracker::~Tracker()
{
    std::promise<void> stoppedPromise;
    m_pollable.dispatch(
        [this, &stoppedPromise]()
        {
            stopUnsafe();
            stoppedPromise.set_value();
        });
    stoppedPromise.get_future().wait();
    NX_VERBOSE(this, "Destroyed tracker.");
}

void Tracker::stopUnsafe()
{
    if (!m_webSocket)
        return;
    m_webSocket.reset();
    // Dangerous: object does not exists after this call.
    m_session->releaseTracker();
}

void Tracker::start(
    std::unique_ptr<AbstractStreamSocket>&& socket,
    const nx::network::http::HttpHeaders& headers,
    nx::network::websocket::Role wsRole)
{
    socket->setNonBlockingMode(true);
    //< No bindToAioThread here: the tracker was constructed on this socket's
    //< own thread. Moving a socket that is already live - a relayed one is fed
    //< by the tunnel from another thread throughout the handover - would leave
    //< its queued posted calls behind on the old thread, where draining them
    //< trips the isInSelfAioThread assert in AioTaskQueue::processPostedCalls.
    m_webSocket.reset(new WebSocket(
        std::move(socket),
        wsRole,
        nx::network::websocket::FrameType::text,
        websocket::compressionType(headers)));
    m_pollable.post(
        [this]()
        {
            m_webSocket->start();
            sendOffer();
            readMessage();
            m_started = true;
        });
}

void Tracker::readMessage()
{
    m_webSocket->readSomeAsync(
        &m_buffer,
        [this](auto... args)
        {
            onBytesRead(std::move(args)...);
        });
}

void Tracker::sendOffer()
{
    // Every offer path funnels through here, so this is the single point that marks a round in
    // flight; add/remove commands then queue until onOfferAnswered() lands the answer.
    m_session->markOfferSent();
    rapidjson::Document d(rapidjson::kObjectType);
    rapidjson::Value sdpRoot(rapidjson::kObjectType);
    sdpRoot.AddMember("type", "offer", d.GetAllocator());
    std::string sdp = m_session->constructSdp();
    rapidjson::Value sdpValue(sdp, d.GetAllocator());
    sdpRoot.AddMember("sdp", sdpValue, d.GetAllocator());
    d.AddMember("sdp", sdpRoot, d.GetAllocator());
    // The peer echoes it with its answer, which is the only way to tell which offer an answer
    // is to: the SDP of an answer names nothing of the offer.
    d.AddMember("offerId", m_session->offerVersion(), d.GetAllocator());
    sendJson(d);
}

void Tracker::renegotiateCodec(nx::Uuid deviceId)
{
    m_pollable.dispatch(
        [this, deviceId]()
        {
            m_session->beginCodecRenegotiation(deviceId);
        });
}

void Tracker::sendJson(const rapidjson::Document& doc)
{
    nx::Buffer buf;
    rapidjson::StringBuffer b;
    rapidjson::Writer<rapidjson::StringBuffer> w(b);
    doc.Accept(w);
    std::string s(b.GetString(), b.GetSize());
    NX_VERBOSE(this, "Send json: %1", s);
    buf.resize(s.size());
    memcpy(buf.begin(), s.data(), s.size());
    m_webSocket->sendAsync(
        &buf,
        [this](SystemError::ErrorCode error, size_t /*size*/)
        {
            if (error != SystemError::noError)
            {
                NX_VERBOSE(this, "Got send error %1", SystemError::toString(error));
                stopUnsafe();
            }
        });
}

void Tracker::sendUnsupportedCodecs()
{
    for (const auto& [deviceId, codecId]: m_session->unsupportedVideoCodecs())
    {
        rapidjson::Document d(rapidjson::kObjectType);
        d.AddMember("error", "codecUnsupported", d.GetAllocator());
        rapidjson::Value deviceValue(deviceId.toSimpleStdString(), d.GetAllocator());
        d.AddMember("deviceId", deviceValue, d.GetAllocator());
        //< FFmpeg's short name, e.g. "hevc"; the client maps it to a display name.
        rapidjson::Value codecValue(avcodec_get_name(codecId), d.GetAllocator());
        d.AddMember("codec", codecValue, d.GetAllocator());
        sendJson(d);
    }
}

void Tracker::sendIce(const IceCandidate& candidate)
{
    if (!candidate.matchFilter(m_candidateFilter))
        return;
    auto candidateStr = candidate.serialize();
    rapidjson::Document d(rapidjson::kObjectType);
    rapidjson::Value ice(rapidjson::kObjectType);
    rapidjson::Value candidateValue(candidateStr, d.GetAllocator());
    ice.AddMember("candidate", candidateValue, d.GetAllocator());

    //< Candidates attach to the bundle's first m-line - the data channel leads every offer.
    ice.AddMember("sdpMid", kDataChannelMid, d.GetAllocator());
    ice.AddMember("sdpMLineIndex", 0, d.GetAllocator());
    std::string ufrag = m_session->id();
    rapidjson::Value ufragValue(ufrag, d.GetAllocator());
    ice.AddMember("usernameFragment", ufragValue, d.GetAllocator());
    d.AddMember("ice", ice, d.GetAllocator());
    sendJson(d);
}

AnswerResult Tracker::examineAnswer()
{
    rapidjson::Document d;
    const auto s = m_buffer.toString();
    d.Parse(s.data());
    if (!d.IsObject())
        return AnswerResult::noop;
    if (d.HasMember("ice"))
    {
        const auto& ice = d["ice"];
        if (ice.IsObject() && ice.HasMember("candidate"))
        {
            const auto& candidate = ice["candidate"];
            if (candidate.IsString())
            {
                // The tracker only parses; which candidate types drive what is
                // ICE policy and lives in Session::gotIceFromTracker.
                m_session->gotIceFromTracker(IceCandidate::parse(candidate.GetString()));
            }
        }
    }
    if (d.HasMember("error"))
        return m_session->setFallbackCodecs();
    if (!d.HasMember("sdp"))
        return AnswerResult::noop; //< Case with remote ice-candidate received.
    const auto& sdpRoot = d["sdp"];
    if (!sdpRoot.IsObject() || !sdpRoot.HasMember("type") || !sdpRoot.HasMember("sdp"))
        return AnswerResult::noop;
    const auto& msgType = sdpRoot["type"];
    if (!msgType.IsString() || msgType.GetString() != std::string("answer"))
        return AnswerResult::noop;
    const auto& sdp = sdpRoot["sdp"];
    if (!sdp.IsString())
        return AnswerResult::noop;
    // An offer sent before its predecessor was answered leaves that answer describing an older
    // set of tracks. Judged against the current one, every track minted since reads as rejected
    // by the peer - a healthy camera then ends up reported as a codec the browser cannot play.
    // A peer that echoes no id is taken at its word.
    if (d.HasMember("offerId") && d["offerId"].IsInt()
        && d["offerId"].GetInt() != m_session->offerVersion())
    {
        NX_DEBUG(
            this,
            "%1: ignoring the answer to offer %2, offer %3 is out",
            m_session->id(),
            d["offerId"].GetInt(),
            m_session->offerVersion());
        return AnswerResult::noop;
    }
    return m_session->processSdpAnswer(sdp.GetString());
}

void Tracker::flushIceCandidatesOnce()
{
    if (m_iceSent)
        return;
    const auto iceCandidates = m_session->getIceCandidates();
    for (const auto& candidate: iceCandidates)
        sendIce(candidate);
    m_iceSent = true;
}

bool Tracker::processMessages()
{
    /* http://rtcweb-wg.github.io/jsep/
     * JSEP's handling of session descriptions is simple and straightforward.
     * Whenever an offer/answer exchange is needed, the initiating side creates an offer
     * by calling a createOffer() API. The application then uses that offer to set up its
     * local config via the setLocalDescription() API. The offer is finally sent off to the
     * remote side over its preferred signaling mechanism (e.g., WebSockets); upon receipt
     * of that offer, the remote party installs it using the setRemoteDescription() API.
     * To complete the offer/answer exchange, the remote party uses the createAnswer() API
     * to generate an appropriate answer, applies it using the setLocalDescription() API,
     * and sends the answer back to the initiator over the signaling channel. When the initiator
     * gets that answer, it installs it using the setRemoteDescription() API, and initial setup
     * is complete. This process can be repeated for additional offer/answer exchanges.
     */

    AnswerResult result = examineAnswer();
    NX_VERBOSE(this, "Answer examination result: %1", (int) result);
    if (result == AnswerResult::failed)
        return false;
    // A camera the peer cannot decode is that camera's problem alone: name it, and leave the
    // socket and the session to the cameras that do play.
    if (result != AnswerResult::noop)
        sendUnsupportedCodecs();
    if (result == AnswerResult::again)
    {
        sendOffer();
        return true;
    }

    //< A Purpose::data session has no consumer and nothing to start.
    if (auto* consumer = m_session->consumer())
    {
        consumer->startProvidersIfNeeded();

        //< A successful answer to a codec-change offer: let the consumer resume,
        //< re-feeding the held packet to the freshly rebuilt encoder. Gated on
        //< `success` (not `noop`) so a stray trickle-ICE message during the
        //< renegotiation window can't resume media before the answer lands.
        if (result == AnswerResult::success)
            consumer->resumeAfterRenegotiation();
    }

    // The round has landed: apply any add/remove that queued while this offer was in flight and
    // send one coalesced offer for them. `noop` is a non-answer (trickle ICE) and ends no round;
    // a session whose every camera the peer rejected did answer, and must still be able to take
    // the next one.
    if (result == AnswerResult::success || result == AnswerResult::unsupportedCodec)
        m_session->onOfferAnswered();

    flushIceCandidatesOnce();
    return true;
}

void Tracker::onIceCandidate(const IceCandidate& candidate)
{
    // Can be called from another thread.
    m_pollable.dispatch(
        [this, candidate]()
        {
            if (!m_iceSent)
                return; //< All ice will be send later.
            sendIce(candidate);
        });
}

void Tracker::onBytesRead(SystemError::ErrorCode errorCode, std::size_t /*bytesTransferred*/)
{
    if (errorCode != SystemError::noError || !processMessages())
    {
        stopUnsafe();
    }
    else
    {
        m_buffer.clear();
        readMessage();
    }
}

std::string Tracker::idForToStringFromPtr() const
{
    return m_sessionId;
}

} // namespace nx::webrtc
