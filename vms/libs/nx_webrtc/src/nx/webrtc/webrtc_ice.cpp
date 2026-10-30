// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "webrtc_ice.h"

#include <QtCore/QtEndian>

#include <nx/network/stun/message_integrity.h>

#include "webrtc_session.h"
#include "webrtc_session_pool.h"

namespace nx::webrtc {

using namespace nx::vms::api;
using namespace nx::network;
using namespace nx;

// Depth below which the media pump may run in parallel with the data channel
// instead of waiting for it to go idle. Zero would serialise the two streams
// completely: every metadata message would hold the pump until usrsctp accepted
// it. A shallow backlog is not congestion, so allow it through and keep the gate
// for the case that matters - a queue that is genuinely building up.
//
// Counted in messages, which is a rough proxy: writeMessage() fragments anything
// over 16 KiB, so one large best shot occupies several entries while several
// small object packets occupy one each. Worth revisiting as a byte budget once
// the observed depth distribution is known.
constexpr int kMaxDataQueueForParallelSend = 4;

nx::Buffer buildControlledBindingRequest(
    const std::string& remoteUfrag,
    const std::string& localUfrag,
    const std::string& remotePwd,
    int priority,
    std::uint64_t tieBreaker)
{
    stun::Message request(
        stun::Header(stun::MessageClass::request, stun::MethodType::bindingMethod));

    request.newAttribute<stun::attrs::Priority>(priority);

    // ICE-CONTROLLED carries the agent's tie-breaker as a 64-bit value in network byte order.
    const std::uint64_t tieBreakerBe = qToBigEndian(tieBreaker);
    request.newAttribute<stun::attrs::IceControlled>(
        nx::Buffer(reinterpret_cast<const char*>(&tieBreakerBe), sizeof(tieBreakerBe)));

    // USERNAME is "<peer ufrag>:<our ufrag>"; MESSAGE-INTEGRITY MUST be keyed with
    // the peer's (remote) ice-pwd so the peer can verify the check.
    const std::string username = remoteUfrag + ":" + localUfrag;
    request.insertIntegrity(username, remotePwd);

    stun::MessageSerializer serializer;
    serializer.setAlwaysAddFingerprint(true);
    return serializer.serialized(request);
}

Ice::Ice(SessionPool* sessionPool, const SessionConfig& config, aio::AbstractAioThread* aioThread):
    AbstractDataChannelDelegate(config),
    m_sessionPool(sessionPool)
{
    //< Safe here and nowhere later: m_pollable is not reachable yet, so no
    //< posted call can be stranded on the thread we are leaving. Subclasses
    //< bind their sockets and timers after us, onto the thread chosen here.
    if (aioThread)
        m_pollable.bindToAioThread(aioThread);
}

Ice::~Ice()
{
}

bool Ice::hasBufferToSend() const
{
    NX_ASSERT(m_pollable.isInSelfAioThread());
    return m_hasBufferToSend;
}

nx::Buffer Ice::toSendBuffer(const char* data, int dataSize) const
{
    return nx::Buffer(data, dataSize);
}

void Ice::sendStunError(const stun::Message& message)
{
    stun::Message response;
    response.header.messageClass = stun::MessageClass::errorResponse;
    response.header.method = message.header.method;
    response.header.transactionId = message.header.transactionId;

    stun::MessageSerializer stunSerializer;
    nx::Buffer sendBuffer = stunSerializer.serialized(response);

    writePacket(sendBuffer.data(), sendBuffer.size(), /*foreground*/ true);
}

Ice::PacketResult Ice::handleStun(const uint8_t* data, int size)
{
    // 1. Parse
    nx::Buffer buffer((const char*) data, size);
    stun::Message message;
    stun::MessageParser stunParser;
    stunParser.setFingerprintRequired(true);
    stunParser.setMessage(&message);
    size_t parsed = 0;
    if (stunParser.parse(buffer, &parsed) != nx::network::server::ParserState::done)
    {
        NX_VERBOSE(this, "STUN message not parsed");
        return PacketResult::drop;
    }

    // 2. Check
    if ((message.header.messageClass != stun::MessageClass::request
         && message.header.messageClass != stun::MessageClass::successResponse)
        || message.header.method != stun::MethodType::bindingMethod)
    {
        NX_VERBOSE(this, "STUN binding message wrong header");
        return PacketResult::drop;
    }

    if (message.header.messageClass == stun::MessageClass::successResponse)
    {
        // A binding success response (the peer's reply to our connectivity
        // check or keepalive, or a stray STUN-server reply) carries no
        // USERNAME, so it can't be authenticated and there is nothing to
        // answer. But while this ice is still in the binding stage it is the
        // earliest evidence that the direct path works end-to-end - report it
        // to the session so the relay fallback gives the peer's own check more
        // time instead of releasing the deferred private candidates over an
        // in-progress direct handshake. Unauthenticated, so it may only delay
        // the fallback, never cancel it (see onDirectPathProbeSucceeded).
        if (m_stage == Stage::binding && !m_punchSessionId.empty())
        {
            if (auto s = m_sessionPool->getSession(m_punchSessionId).lock())
                s->onDirectPathProbeSucceeded();
        }
        NX_VERBOSE(this, "STUN: binding success response");
        return PacketResult::drop;
    }

    auto iceControlling = message.getAttribute<stun::attrs::IceControlling>();
    auto iceControlled = message.getAttribute<stun::attrs::IceControlled>();
    auto useCandidate = message.getAttribute<stun::attrs::UseCandidate>();

    // Next 2 checks are not exactly valid due to client role is unimplemented.
    if (!!iceControlling == !!iceControlled)
    {
        NX_VERBOSE(
            this,
            "STUN unexpected 'ice-controlling' and 'ice-controlled': %1 vs %2",
            !!iceControlling,
            !!iceControlled);
    }

    bool controlling = (iceControlling == nullptr);
    if (controlling && !!useCandidate)
    {
        NX_VERBOSE(
            this,
            "STUN unexpected 'controlling' and 'use-candidate': %1 vs %2",
            controlling,
            !!useCandidate);
    }
    auto priority = message.getAttribute<stun::attrs::Priority>();
    if (!priority && message.header.messageClass == stun::MessageClass::request)
    {
        NX_VERBOSE(this, "STUN no PRIORITY attribute");
        sendStunError(message);
        return PacketResult::drop;
    }
    // TODO Provide additional checks: priority should match with priority from SDP.

    // USERNAME from STUN is in format "<local ice-ufrag>:<remote ice-ufrag>"
    auto username = message.getAttribute<stun::attrs::UserName>();
    if (!username)
    {
        NX_VERBOSE(this, "STUN no USERNAME attribute");
        sendStunError(message);
        return PacketResult::drop;
    }

    std::string stunUsername = username->getString();
    auto colonPos = stunUsername.find(":");
    if (colonPos == std::string::npos)
    {
        NX_VERBOSE(this, "STUN wrong USERNAME attribute format");
        sendStunError(message);
        return PacketResult::drop;
    }
    auto leftUsername = stunUsername.substr(0, colonPos);
    auto rightUsername = stunUsername.substr(colonPos + 1);

    //< Set for IceTcp when the first packet names a session on another aio thread; the
    //< deferred migration below (once the packet is authenticated) reads it.
    aio::AbstractAioThread* sessionThread = nullptr;
    if (!m_sessionDescription)
    {
        auto sessionPtr = m_sessionPool->getSession(leftUsername);
        auto session = sessionPtr.lock();
        if (!session)
        {
            NX_DEBUG(this, "STUN session %1 was not found", leftUsername);
            sendStunError(message);
            return PacketResult::drop;
        }
        m_sessionDescription = session->describe();
        if (!m_sessionDescription)
        {
            NX_DEBUG(this, "STUN session %1 is locked or expired", leftUsername);
            sendStunError(message);
            return PacketResult::drop;
        }
        //< The thread outlives the session, so the pointer is safe even if the session
        //< goes away before the move happens.
        sessionThread = session->aioThread();
    }
    else if (m_sessionDescription->id != leftUsername)
    {
        NX_DEBUG(
            this,
            "STUN heartbeat with mismatch USERNAME: %1 vs %2",
            m_sessionDescription->id,
            leftUsername);
        sendStunError(message);
        return PacketResult::drop;
    }
    if (m_sessionDescription->remoteUfrag != rightUsername)
    {
        NX_DEBUG(
            this,
            "STUN session: right username mismatch: %1 vs %2",
            rightUsername,
            m_sessionDescription->remoteUfrag);
        sendStunError(message);
        return PacketResult::drop;
    }

    if (!message.verifyIntegrity(stunUsername, m_sessionDescription->localPwd))
    {
        NX_DEBUG(this, "STUN integrity check failed with user %1", stunUsername);
        sendStunError(message);
        return PacketResult::drop;
    }

    // The packet is now an authenticated binding request. Hand the real source
    // address to the subclass so it can learn a peer-reflexive candidate and retarget
    // toward the peer's actual mapping (which may differ from the SDP-advertised srflx).
    onPeerValidated(iceRemoteAddress());

    //< IceTcp's first packet named a session on another thread. Now that the packet is
    //< authenticated, migrate before answering: the STUN response - and the dtls
    //< handshake, session lock and consumer after it - belong on the session's thread,
    //< not this pool one. Return without answering; IceTcp keeps the packet buffered and
    //< replays it there (see processBufferedPackets), so nothing is written on this
    //< thread before the move. IceUdp and IceRelay are already on their session's thread.
    if (sessionThread && sessionThread != m_pollable.getAioThread())
    {
        m_pendingAioThread = sessionThread;
        return PacketResult::valid;
    }

    // 3. Write response
    stun::Message response;
    response.header.messageClass = stun::MessageClass::successResponse;
    response.header.method = stun::MethodType::bindingMethod;
    response.header.transactionId = message.header.transactionId;

    // XOR-MAPPED-ADDRESS for both families. libwebrtc discards a binding
    // response that lacks it as a failed check, which silently broke IPv6 host
    // candidate pairs (advertised, but every browser-side check failed). The
    // attribute and serializer already encode v6; the caller was just v4-only.
    // The SocketAddress overload handles both families.
    response.newAttribute<stun::attrs::XorMappedAddress>(iceRemoteAddress());

    if (controlling)
        response.newAttribute<stun::attrs::UseCandidate>();

    // MESSAGE-INTEGRITY alone, keyed with this agent's pwd (short-term
    // credentials, same key as the request) - NOT insertIntegrity(), which
    // adds USERNAME and NONCE, and NOT a REALM. A response MUST NOT carry
    // USERNAME (RFC 5389 10.1.2), and a REALM switches a strict peer - the
    // toolbox's libjuice - to long-term credential key derivation
    // (MD5(user:realm:pass)), making every response unverifiable: the peer
    // then retransmits its checks forever and never nominates. Browsers
    // tolerated the old nonstandard shape; libjuice does not.
    response.newAttribute<stun::attrs::MessageIntegrity>(
        nx::Buffer(stun::attrs::MessageIntegrity::SIZE, 0));
    response.newAttribute<stun::attrs::MessageIntegrity>(
        stun::calcMessageIntegrity(response, m_sessionDescription->localPwd, {}));

    stun::MessageSerializer stunSerializer;
    stunSerializer.setAlwaysAddFingerprint(true);
    nx::Buffer sendBuffer = stunSerializer.serialized(response);

    writePacket(sendBuffer.data(), sendBuffer.size(), /*foreground*/ true);

    NX_DEBUG(this, "STUN handshake successful for session %1", m_sessionDescription->id);

    // 4. Change stage
    if (m_stage == Stage::binding)
    {
        m_stage = Stage::dtls;
        if (auto s = m_sessionPool->getSession(m_sessionDescription->id).lock())
            s->onDirectConnectivityEstablished();
    }

    return PacketResult::valid;
}

void Ice::writeDataChannelPacket(const uint8_t* data, int size)
{
    nx::Buffer buffer((const char*) data, size);

    m_pollable.dispatch(
        [this, buffer = std::move(buffer)]()
        {
            if (m_stage == Stage::streaming && m_sessionDescription)
            {
                // TODO probably mutex in DTLS can be removed.
                if (Dtls::Status::error
                    == m_sessionDescription->dtls
                           ->writePacket((const uint8_t*) buffer.data(), buffer.size(), this))
                {
                    NX_DEBUG(
                        this,
                        "%1: Failed to write datachannel packet",
                        m_sessionDescription->id);
                }
            }
        });
}

void Ice::writeDtlsPacket(const char* data, int size)
{
    writePacket(data, size, /*foreground*/ true);
}

bool Ice::onDtlsData(const uint8_t* data, int size)
{
    return m_dataChannel.handlePacket(data, size);
}

Ice::PacketResult Ice::handleDtls(const uint8_t* data, int size)
{
    NX_CRITICAL(m_sessionDescription);
    auto status = m_sessionDescription->dtls->handlePacket(data, size, this);
    if (status == Dtls::Status::error)
        return PacketResult::fatal;
    if (status == Dtls::Status::streaming && m_stage == Stage::dtls)
    {
        if (m_needStop)
            return PacketResult::fatal;

        m_stage = Stage::streaming;

        /* Some peers (at least Firefox) make successfully STUN handshake into several ICE
         * candidates in parallel, and then choose one of them to make DTLS handshake. This one
         * chosen with remote peer can mismatch with one who first locked session on the server
         * side. So, lock session afterward, on successful DTLS handshake.
         * */
        NX_CRITICAL(!m_session.lock());
        auto sessionPtr = m_sessionPool->getSession(m_sessionDescription->id);
        auto session = sessionPtr.lock();
        if (!session)
        {
            NX_DEBUG(this, "Init DTLS: session %1 was not found", m_sessionDescription->id);
            return PacketResult::fatal;
        }
        if (!session->lock())
        {
            NX_DEBUG(this, "Init DTLS: can't lock session %1", m_sessionDescription->id);
            return PacketResult::fatal;
        }
        else
        {
            NX_DEBUG(this, "Session %1 is locked", m_sessionDescription->id);
        }
        m_session = sessionPtr;
        session->setActiveIce(this);

        m_dataChannel.openDataChannel("nx-channel", "");

        // Start streaming.
        if (!session->transceiver()->start(this, m_sessionDescription->dtls.get()))
            return PacketResult::fatal;
    }
    return PacketResult::valid;
}

Ice::PacketResult Ice::handleSrtp(const uint8_t* data, int size)
{
    auto session = m_session.lock();
    if (!session)
        return PacketResult::drop; //< SRTP before the session is locked; ignore.

    std::vector<uint8_t> buffer;
    buffer.assign(data, data + size);

    return session->transceiver()->onSrtp(std::move(buffer)) ? PacketResult::valid
                                                             : PacketResult::fatal;
}

Ice::Stage Ice::demuxMessageStage(const uint8_t* data, size_t size)
{
    NX_CRITICAL(size >= 2);
    if (((data[0] == 0x0 || data[0] == 0x1) && data[1] == 0x1) || m_stage == Stage::binding)
    {
        /* Actually, second byte 0x1 means binding request.
         * Not all STUN requests begins from 0x0 0x1.
         * Also, binding request is also used as heartbeat in streaming stage */
        return Stage::binding;
    }
    else if (data[1] == 0xfe || m_stage == Stage::dtls)
    {
        return Stage::dtls;
    }
    else if (m_stage == Stage::streaming)
    {
        /* Assume it's RTP/RTCP */
        return Stage::streaming;
    }
    else
    {
        return Stage::unknown;
    }
}

Ice::PacketResult Ice::processIncomingPacket(const uint8_t* data, int size)
{
    // Any packet from the peer (STUN, DTLS, SRTP/RTCP, relay data) proves the
    // peer is still there. Refresh the session's liveness clock so a locked
    // (streaming) session survives; when the peer vanishes these stop and the
    // SessionPool reaper collects it. m_session is set only once the session is
    // locked, so this is a no-op during the handshake (idle timer covers that).
    if (auto session = m_session.lock())
        session->onInboundActivity();

    Stage currentStage;
    if (size < 2 || (currentStage = demuxMessageStage(data, size)) == Stage::unknown)
    {
        NX_DEBUG(
            this,
            "Unexpected packet received for %1",
            (m_sessionDescription ? m_sessionDescription->id : "(null)"));
        return PacketResult::drop; //< Garbage / unrecognised; ignore, keep the ice.
    }

    switch (currentStage)
    {
        case Stage::binding:
            return handleStun(data, size);
        case Stage::dtls:
            return handleDtls(data, size);
        case Stage::streaming:
            return handleSrtp(data, size);
        default:
            NX_ASSERT(false);
            return PacketResult::fatal;
    }
}

void Ice::writePacket(const char* data, int size, bool foreground)
{
    NX_ASSERT(m_pollable.isInSelfAioThread());
    if (foreground)
        insertForegroundBuffer(toSendBuffer(data, size));
    else
        m_sendBuffers.push_back(toSendBuffer(data, size));

    sendNextBufferUnsafe();
}

void Ice::insertForegroundBuffer(nx::Buffer buffer)
{
    NX_ASSERT(m_foregroundOffset <= m_sendBuffers.size());
    m_sendBuffers.insert(m_sendBuffers.begin() + m_foregroundOffset, std::move(buffer));
    ++m_foregroundOffset;
}

void Ice::writeBatch(std::deque<nx::Buffer> mediaBuffers)
{
    NX_ASSERT(m_pollable.isInSelfAioThread());
    // Always non-foreground. Each buffer MUST go through toSendBuffer() so the transport can apply
    // its framing — for TCP this prepends the RFC4571 2-byte length prefix. Pushing raw RTP here
    // (as writePacket does NOT) desyncs the peer's TCP framing: it reads the RTP header as a
    // length and loses the stream, which kills all media AND the interleaved STUN/DTLS on the same
    // socket. For UDP (and the base) toSendBuffer is a passthrough, so this is a no-op there.
    for (auto& packet: mediaBuffers)
        m_sendBuffers.push_back(toSendBuffer(packet.data(), packet.size()));
    sendNextBufferUnsafe();
}

aio::AbstractAioThread* Ice::getAioThread()
{
    return m_pollable.getAioThread();
}

void Ice::onBytesWritten(SystemError::ErrorCode errorCode, std::size_t /*bytesTransferred*/)
{
    if (m_needStop)
        return;

    if (nx::network::socketCannotRecoverFromError(errorCode))
    {
        NX_DEBUG(
            this,
            "%1: got socket write error: %2",
            (m_sessionDescription ? m_sessionDescription->id : "(null)"),
            SystemError::toString(errorCode));
        stopUnsafe();
        return;
    }

    NX_ASSERT(m_pollable.isInSelfAioThread());
    m_hasBufferToSend = false;
    sendNextBufferUnsafe();
}

void Ice::sendNextBufferUnsafe()
{
    if (m_hasBufferToSend)
        return;

    if (m_needStop)
        return;

    if (m_sendBuffers.empty())
    {
        // Ack only once BOTH streams are idle. Waiting for the data channel too
        // is deliberate: video and its metadata are interleaved in timestamp
        // order by the reorder buffer and travel through one media queue, so
        // letting the pump run ahead while metadata is still queued would send
        // frames past the annotations that belong to them. Gating here keeps the
        // send rate matched to whichever stream is slower.
        //
        // The reason this previously froze video for seconds is that it was only
        // a level check: if the queue happened to be non-empty at this instant
        // the ack was simply lost, and nothing re-checked until an unrelated
        // event re-entered the pump. onDataChannelQueueDrained() now supplies
        // the missing edge, so the ack fires the moment the backlog clears.
        if (auto session = m_session.lock())
        {
            if (session->consumer() && !isDataChannelBacklogDeep())
                session->consumer()->onDataSent(/*success*/ true);
        }
        return;
    }

    // The buffer leaves the queue for the duration of the send: the socket keeps a pointer to it
    // until completion, and a deque insert would move it under that pointer.
    m_sendingBuffer = std::move(m_sendBuffers.front());
    m_sendBuffers.pop_front();
    if (m_foregroundOffset != 0)
        --m_foregroundOffset;
    m_hasBufferToSend = true;
    asyncSendPacketUnsafe();
}

void Ice::stopUnsafe()
{
    m_needStop = true;
    m_dataChannel.reset();
    m_pollable.pleaseStopSync();
    if (auto session = m_session.lock())
    {
        session->setActiveIce(nullptr);
        if (session->consumer())
            session->consumer()->stopUnsafe(); //< Consumer::stopUnsafe stops the streamer.
        else
            session->transceiver()->stop(); //< No consumer chain: notify directly (DataPeer).
        session->unlock();
        m_session.reset();
    }
}

void Ice::onDataChannelString(const std::string& data, int streamId)
{
    NX_DEBUG(this, "Got string from Data Channel: %1", data);
    if (auto session = m_session.lock())
        session->transceiver()->onDataChannelString(data, streamId);
}

void Ice::onDataChannelBinary(const std::string& data, int streamId)
{
    NX_DEBUG(this, "Got binary from Data Channel: %1", data);
    if (auto session = m_session.lock())
        session->transceiver()->onDataChannelBinary(data, streamId);
}

void Ice::writeDataChannelString(
    const std::string& data,
    const nx::Uuid& deviceId,
    DataChannel::Delivery delivery)
{
    m_pollable.dispatch(
        [this, data, deviceId, delivery]()
        {
            if (!m_dataChannel.writeString(data, /*streamId*/ -1, deviceId, delivery))
                stopUnsafe();
        });
}

void Ice::writeDataChannelBinary(
    nx::Buffer data,
    const nx::Uuid& deviceId,
    DataChannel::Delivery delivery)
{
    m_pollable.dispatch(
        [this, data = std::move(data), deviceId, delivery]() mutable
        {
            const bool isWritten = m_dataChannel.writeBinary(
                data.takeStdString(), /*streamId*/ -1, deviceId, delivery);
            if (!isWritten)
                stopUnsafe();
        });
}

void Ice::clearDataChannelStrings(const nx::Uuid& deviceId)
{
    m_pollable.dispatch(
        [this, deviceId]()
        {
            const int removed = m_dataChannel.removeQueuedStrings(deviceId);
            if (removed > 0)
                NX_DEBUG(this, "Removed %1 queued data-channel messages of %2", removed, deviceId);
        });
}

int Ice::bufferEmpty() const
{
    return !m_hasBufferToSend && m_sendBuffers.empty() && m_dataChannel.outputQueueSize() == 0;
}

void Ice::onDataChannelOpened(int streamId)
{
    NX_DEBUG(
        this,
        "%1: Data channel opened, stream %2",
        (m_sessionDescription ? m_sessionDescription->id : "(null)"),
        streamId);
    if (auto session = m_session.lock())
        session->transceiver()->onDataChannelOpened(streamId);
}

bool Ice::isDataChannelBacklogDeep() const
{
    return m_dataChannel.outputQueueSize() >= kMaxDataQueueForParallelSend;
}

void Ice::onDataChannelQueueProgress()
{
    auto session = m_session.lock();

    // Producers gated on the backlog (DataPeer) re-evaluate on every shrink,
    // independently of the RTP send state below.
    if (session)
        session->transceiver()->onDataChannelQueueProgress(m_dataChannel.outputQueueSize());

    // The data channel backlog shrank. If RTP is still busy, its own completion
    // will re-evaluate the gate, so there is nothing to do - acking here would
    // clear the consumer's in-progress flag while a send is still outstanding
    // and let a second send start on top of it.
    if (m_hasBufferToSend || !m_sendBuffers.empty())
        return;

    if (isDataChannelBacklogDeep())
        return; //< Still too deep to let the pump run.

    if (session && session->consumer())
        session->consumer()->onDataSent(/*success*/ true);
}

} // namespace nx::webrtc
