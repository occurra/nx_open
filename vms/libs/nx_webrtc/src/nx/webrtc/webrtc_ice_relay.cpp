// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "webrtc_ice_relay.h"

#include <algorithm>

#include <nx/network/system_socket.h>
#include <nx/utils/cryptographic_hash.h>

#include "webrtc_session_pool.h"

using namespace std::chrono;

namespace nx::webrtc {

namespace {

constexpr size_t kRawReadBufferSize = 65536;

constexpr auto kKeepAliveTimeout = 15s;

// Can't detect permissions lifetime in runtime,
// but default lifetime in coturn is 300s.
constexpr auto kPermissionsLifetime = 295s;

constexpr auto kTurnConnectTimeout = 10s;

std::unique_ptr<nx::network::AbstractCommunicatingSocket> makeSocket(
    nx::network::TransportProtocol transport)
{
    if (transport == nx::network::TransportProtocol::tcp)
        return std::make_unique<nx::network::TCPSocket>();

    return std::make_unique<nx::network::UDPSocket>();
}

} // namespace

IceRelay::IceRelay(
    SessionPool* sessionPool,
    const SessionConfig& config,
    const std::string& sessionId,
    const CandidateHandler& handler,
    aio::AbstractAioThread* aioThread):
    Ice(sessionPool, config, aioThread),
    m_sessionId(sessionId),
    m_onCandidateReadyHandler(handler)
{
    m_readBuffer.reserve(kRawReadBufferSize);
    m_keepAliveTimer.bindToAioThread(m_pollable.getAioThread());
    m_createPermissionTimer.bindToAioThread(m_pollable.getAioThread());
    m_refreshTimer.bindToAioThread(m_pollable.getAioThread());
}

IceRelay::~IceRelay()
{
    std::promise<void> stoppedPromise;
    m_pollable.dispatch(
        [this, &stoppedPromise]()
        {
            stopUnsafe();
            stoppedPromise.set_value();
        });
    stoppedPromise.get_future().wait();
}

void IceRelay::stopUnsafe()
{
    if (!m_needStop)
    {
        m_keepAliveTimer.cancelSync();
        m_createPermissionTimer.cancelSync();
        m_refreshTimer.cancelSync();

        if (m_socket)
            m_socket->cancelIOSync(aio::EventType::etAll);

        Ice::stopUnsafe(); //< Dangerous, object can be destroyed after this call.
    }
}

IceCandidate::Filter IceRelay::type() const
{
    return IceCandidate::Filter::UdpRelay;
}

void IceRelay::startRelay(const TurnServerInfo& turnServer)
{
    m_turnServer = turnServer;
    m_endpointIndex = 0;
    connectCurrentEndpoint();
}

void IceRelay::connectCurrentEndpoint()
{
    if (m_needStop)
        return;

    if (m_endpointIndex >= m_turnServer.endpoints.size())
    {
        NX_DEBUG(
            this,
            "TURN: all %1 endpoint(s) failed for %2",
            m_turnServer.endpoints.size(),
            m_sessionId);
        stopUnsafe();
        return;
    }

    const auto& endpoint = m_turnServer.endpoints[m_endpointIndex];
    NX_DEBUG(
        this,
        "TURN: dialing endpoint %1 (%2) for %3",
        endpoint.address,
        endpoint.transport == nx::network::TransportProtocol::tcp ? "tcp" : "udp",
        m_sessionId);

    m_relayStage = RelayStage::Binding; //< Reset the protocol state machine for this attempt.
    // m_socket is only (re)assigned here, during the pre-candidate connect/failover
    // phase. The send path (asyncSendPacketUnsafe), which reads m_socket off the AIO
    // thread, becomes active only after a relay candidate is reported - past the last
    // reassignment - so it never observes a concurrent change. Keep that invariant if
    // sending is ever moved earlier.
    m_socket = makeSocket(endpoint.transport);
    m_socket->bindToAioThread(m_pollable.getAioThread());
    m_socket->setSendBufferSize(kIceTcpSendBufferSize);
    m_socket->setSendTimeout(
        milliseconds(kTurnConnectTimeout).count()); //< connectAsync() uses the send timeout.
    m_socket->setNonBlockingMode(true);

    m_socket->connectAsync(
        endpoint.address,
        [this](SystemError::ErrorCode errorCode)
        {
            if (m_needStop)
                return;

            if (errorCode != SystemError::noError)
            {
                ++m_endpointIndex;
                // Defer so the failed socket's own callback unwinds before we
                // replace it in connectCurrentEndpoint(); both run on the same
                // AIO thread.
                m_pollable.post(
                    [this]()
                    {
                        connectCurrentEndpoint();
                    });
            }
            else
            {
                bindToRelay();
            }
        });
}

void IceRelay::addRemotePeer(const IceCandidate& candidate)
{
    // Guarded by IceManager, so can't be called in parallel with ~IceRelay().
    m_pollable.dispatch(
        [this, candidate]()
        {
            if (m_needStop)
                return;

            if (candidate.type == IceCandidate::Type::Srflx)
                m_remoteSrflx = candidate;

            // Permissions are per-IP: a second candidate on an already
            // permitted IP (e.g. another relay port on the same TURN server)
            // adds nothing and must not resend the request.
            const bool alreadyPermitted = std::any_of(
                m_permittedPeers.begin(),
                m_permittedPeers.end(),
                [&candidate](const nx::network::SocketAddress& address)
                {
                    return address.address == candidate.address.address;
                });
            if (alreadyPermitted)
                return;
            m_permittedPeers.push_back(candidate.address);

            // Before the allocation completes the addresses just accumulate -
            // handleAllocate() issues the request. Past that point every new
            // IP needs a fresh CreatePermission covering the whole set
            // (permissions are additive on the server, RFC 5766 #9.1).
            if (m_relayStage == RelayStage::CreatePermission
                || m_relayStage == RelayStage::Indication)
            {
                NX_DEBUG(
                    this,
                    "TURN: create permission for %1 peer address(es), session %2",
                    m_permittedPeers.size(),
                    m_sessionId);
                createPermission();
            }
        });
}

void IceRelay::bindToRelay()
{
    stun::Message request(
        stun::Header(stun::MessageClass::request, stun::MethodType::bindingMethod));

    stun::MessageSerializer stunSerializer;
    stunSerializer.setAlwaysAddFingerprint(false);
    nx::Buffer sendBuffer = stunSerializer.serialized(request);
    writeRelayPacket(sendBuffer.data(), sendBuffer.size());

    NX_DEBUG(this, "TURN: binding sent for session %1", m_sessionId);

    if (NX_ASSERT(m_socket))
    {
        m_socket->readSomeAsync(
            &m_readBuffer,
            [this](auto... args)
            {
                onBytesRead(std::move(args)...);
            });
    }
}

nx::Buffer IceRelay::toSendBuffer(const char* data, int dataSize) const
{
    if (dataSize > std::numeric_limits<uint16_t>::max())
    {
        NX_WARNING(this, "Can't send message: size is too large %1 for %2", dataSize, m_sessionId);
        return nx::Buffer();
    }

    NX_CRITICAL(m_peerAddress || !m_permittedPeers.empty());
    stun::Message response(
        stun::Header(stun::MessageClass::indication, stun::MethodType::sendMethod));
    // Prefer the address the peer's own packets actually arrived from (its
    // NAT may have rewritten the port, or the peer may be reaching us through
    // its relay rather than the srflx it signaled); before any packet arrives,
    // fall back to its srflx, then to the first permitted address (a
    // relay-only peer signals no srflx at all).
    if (m_peerAddress)
        response.newAttribute<stun::attrs::XorPeerAddress>(*m_peerAddress);
    else if (m_remoteSrflx)
        response.newAttribute<stun::attrs::XorPeerAddress>(m_remoteSrflx->address);
    else
        response.newAttribute<stun::attrs::XorPeerAddress>(m_permittedPeers.front());
    response.newAttribute<stun::attrs::Data>(nx::Buffer(data, dataSize));

    stun::MessageSerializer stunSerializer;
    stunSerializer.setAlwaysAddFingerprint(false);
    return stunSerializer.serialized(response);
}

void IceRelay::fillAuthorizationAndIntegrity(stun::Message& message)
{
    switch (m_turnAuth)
    {
        case AuthorizationType::None:
            return;
        case AuthorizationType::LongTerm:
            message.newAttribute<stun::attrs::UserName>(m_turnServer.user);
            message.newAttribute<stun::attrs::Realm>(m_realm);
            message.newAttribute<stun::attrs::Nonce>(m_nonce);
            message.insertIntegrity(m_turnServer.user, m_password);
            return;
    }
}

void IceRelay::createPermission()
{
    NX_CRITICAL(!m_permittedPeers.empty());

    stun::MessageSerializer stunSerializer;
    stunSerializer.setAlwaysAddFingerprint(false);
    stun::Message request(
        stun::Header(stun::MessageClass::request, stun::MethodType::createPermissionMethod));
    // One XOR-PEER-ADDRESS per permitted IP (RFC 5766 #9 allows several per
    // request): the peer's srflx plus its relayed address(es), so its checks
    // pass whichever public identity they arrive from.
    for (const auto& peer: m_permittedPeers)
        request.newAttribute<stun::attrs::XorPeerAddress>(peer);
    fillAuthorizationAndIntegrity(request);

    nx::Buffer sendBuffer = stunSerializer.serialized(request);
    writeRelayPacket(sendBuffer.data(), sendBuffer.size());
}

void IceRelay::sendRefresh()
{
    stun::MessageSerializer stunSerializer;
    stunSerializer.setAlwaysAddFingerprint(false);
    stun::Message request(
        stun::Header(stun::MessageClass::request, stun::MethodType::refreshMethod));
    request.newAttribute<stun::attrs::Lifetime>(m_refreshLifetimeSeconds);
    fillAuthorizationAndIntegrity(request);

    nx::Buffer sendBuffer = stunSerializer.serialized(request);
    writeRelayPacket(sendBuffer.data(), sendBuffer.size());
}

void IceRelay::scheduleRefresh()
{
    auto refreshLifetime = std::chrono::seconds(std::max(m_refreshLifetimeSeconds - 5, 5));
    NX_VERBOSE(this, "TURN schedule refresh (lifetime=%1) for %2", refreshLifetime, m_sessionId);

    m_refreshTimer.start(
        refreshLifetime,
        [this]()
        {
            NX_VERBOSE(this, "Renew allocation for %1", m_sessionId);
            sendRefresh();
        });
}

bool IceRelay::handleRefresh(const stun::Message& message)
{
    if (message.header.method != stun::MethodType::refreshMethod
        || message.header.messageClass != stun::MessageClass::successResponse)
    {
        NX_WARNING(
            this,
            "TURN unexpected message for Refresh stage: %1/%2 for %3",
            message.header.messageClass,
            message.header.method,
            m_sessionId);
        return false;
    }
    stun::MessageSerializer stunSerializer;
    stunSerializer.setAlwaysAddFingerprint(false);

    auto lifetime = message.getAttribute<stun::attrs::Lifetime>();
    if (lifetime)
        m_refreshLifetimeSeconds = lifetime->value();

    scheduleRefresh();

    return true;
}

bool IceRelay::handleBinding(const stun::Message& message)
{
    if (message.header.messageClass != stun::MessageClass::successResponse
        || message.header.method != stun::MethodType::bindingMethod)
    {
        NX_WARNING(
            this,
            "TURN unexpected message for Binding stage: %1/%2 for %3",
            message.header.messageClass,
            message.header.method,
            m_sessionId);
        return false;
    }
    stun::MessageSerializer stunSerializer;
    stunSerializer.setAlwaysAddFingerprint(false);

    stun::Message request(
        stun::Header(stun::MessageClass::request, stun::MethodType::allocateMethod));
    request.newAttribute<stun::attrs::RequestedTransport>();

    nx::Buffer sendBuffer = stunSerializer.serialized(request);
    writeRelayPacket(sendBuffer.data(), sendBuffer.size());

    m_relayStage = RelayStage::AllocateNoAuth;
    return true;
}

bool IceRelay::handleLongTermAuthorization(const stun::Message& message)
{
    m_turnAuth = AuthorizationType::LongTerm;
    auto nonce = message.getAttribute<stun::attrs::Nonce>();
    if (!nonce)
        m_nonce = nx::Uuid::createUuid().toStdString(QUuid::WithBraces);
    else
        m_nonce = nonce->getString();

    auto realm = message.getAttribute<stun::attrs::Realm>();
    if (!realm)
        m_realm = m_turnServer.realm;
    else
        m_realm = realm->getString();
    if (m_turnServer.longTermAuth)
    {
        m_password = nx::utils::md5(
                         QByteArray::fromStdString(
                             m_turnServer.user + ":" + m_realm + ":" + m_turnServer.password))
                         .toStdString();
    }
    else
    {
        m_password = m_turnServer.password;
    }
    return true;
}

void IceRelay::sendAllocate()
{
    stun::MessageSerializer stunSerializer;
    stunSerializer.setAlwaysAddFingerprint(false);

    stun::Message request(
        stun::Header(stun::MessageClass::request, stun::MethodType::allocateMethod));
    request.newAttribute<stun::attrs::RequestedTransport>();
    fillAuthorizationAndIntegrity(request);

    nx::Buffer sendBuffer = stunSerializer.serialized(request);
    writeRelayPacket(sendBuffer.data(), sendBuffer.size());
}

bool IceRelay::handleAllocateNoAuth(const stun::Message& message)
{
    if (message.header.method != stun::MethodType::allocateMethod)
    {
        NX_WARNING(
            this,
            "TURN unexpected message for AllocateNoAuth stage: %1/%2 for %3",
            message.header.messageClass,
            message.header.method,
            m_sessionId);
        return false;
    }

    m_relayStage = RelayStage::Allocate;

    if (message.header.messageClass == stun::MessageClass::errorResponse)
    {
        // coturn uses long-term auth; fill authorization info once.
        handleLongTermAuthorization(message);

        sendAllocate();
        return true;
    }
    else
    {
        return handleAllocate(message);
    }
}

bool IceRelay::handleAllocate(const stun::Message& message)
{
    if (message.header.method != stun::MethodType::allocateMethod
        || message.header.messageClass != stun::MessageClass::successResponse)
    {
        NX_WARNING(
            this,
            "TURN unexpected message for Allocate stage: %1/%2 for %3",
            message.header.messageClass,
            message.header.method,
            m_sessionId);
        return false;
    }
    stun::MessageSerializer stunSerializer;
    stunSerializer.setAlwaysAddFingerprint(false);

    auto xorMappedAddress = message.getAttribute<stun::attrs::XorMappedAddress>();
    auto xorRelayedAddress = message.getAttribute<stun::attrs::XorRelayedAddress>();
    auto lifetime = message.getAttribute<stun::attrs::Lifetime>();
    if (lifetime)
        m_refreshLifetimeSeconds = lifetime->value();

    if (!xorMappedAddress || !xorRelayedAddress)
    {
        NX_DEBUG(this, "TURN no mapped or relayed address in Allocate response %1", m_sessionId);
        return false;
    }

    // A TURN server behind 1:1 NAT (e.g. coturn on a cloud VM without `external-ip`) can return a
    // private, non-routable XOR-RELAYED-ADDRESS that the remote peer can't reach. In that case
    // keep the allocated relay port but advertise the address of the TURN server we actually
    // connected to — the relay is reachable there through the same NAT. Correctly configured
    // servers return a routable relayed address and are left untouched.
    auto relayedAddr = xorRelayedAddress->addr();
    if (relayedAddr.address.isLocalNetwork() && NX_ASSERT(m_socket))
    {
        const auto turnAddr = m_socket->getForeignAddress();
        if (turnAddr.address.isIpAddress() && !turnAddr.address.isLocalNetwork())
        {
            NX_DEBUG(
                this,
                "TURN relayed address %1 is private; advertising TURN server %2 instead for %3",
                relayedAddr,
                turnAddr,
                m_sessionId);
            relayedAddr.address = turnAddr.address;
        }
    }

    m_localRelay = IceCandidate{
        .protocol = IceCandidate::Protocol::UDP,
        .address = relayedAddr,
        .type = IceCandidate::Type::Relay,
        .relatedAddress = xorMappedAddress->addr()};

    m_relayStage = RelayStage::CreatePermission;
    if (m_permittedPeers.empty())
        NX_VERBOSE(
            this,
            "TURN waiting for a remote peer address to make CreatePermission request %1",
            m_sessionId);
    else
        createPermission();

    scheduleRefresh();

    return true;
}

bool IceRelay::handleCreatePermission(const stun::Message& message)
{
    if (m_permittedPeers.empty())
    {
        NX_WARNING(
            this,
            "TURN unexpected message while waiting for a remote peer address: %1/%2 for %3",
            message.header.messageClass,
            message.header.method,
            m_sessionId);
        return false;
    }

    if (message.header.method != stun::MethodType::createPermissionMethod
        || message.header.messageClass != stun::MessageClass::successResponse)
    {
        NX_WARNING(
            this,
            "TURN unexpected message for CreatePermission stage: %1/%2 for %3",
            message.header.messageClass,
            message.header.method,
            m_sessionId);
        return false;
    }
    if (m_relayStage != RelayStage::Indication)
    {
        NX_DEBUG(
            this,
            "Got ice candidate from manager: %1 -> %2",
            m_sessionId,
            m_localRelay->serialize());
        m_onCandidateReadyHandler(*m_localRelay);
        m_relayStage =
            RelayStage::Indication; //< Ready to send and receive packets to/from remote peer.
    }

    NX_VERBOSE(
        this,
        "TURN schedule renew permissions (lifetime=%1) for %2",
        kPermissionsLifetime,
        m_sessionId);

    m_createPermissionTimer.start(
        kPermissionsLifetime,
        [this]()
        {
            NX_VERBOSE(this, "Renew permissions for %1", m_sessionId);
            createPermission();
        });

    return true;
}

bool IceRelay::handleIndication(const stun::Message& message)
{
    if (message.header.method == stun::MethodType::createPermissionMethod)
    {
        return handleCreatePermission(message);
    }

    if (message.header.method == stun::MethodType::refreshMethod)
    {
        return handleRefresh(message);
    }

    if (message.header.messageClass != stun::MessageClass::indication
        || message.header.method != stun::MethodType::dataMethod)
    {
        NX_DEBUG(
            this,
            "TURN unexpected message in Indication stage: %1/%2 for %3",
            message.header.messageClass,
            message.header.method,
            m_sessionId);
        return true; //< Not a fatal error.
    }
    if (auto peerAddress = message.getAttribute<stun::attrs::XorPeerAddress>())
        m_peerAddress = peerAddress->addr();

    auto data = message.getAttribute<stun::attrs::Data>();
    if (!data)
    {
        NX_DEBUG(this, "TURN no data field in Indication for %1", m_sessionId);
        return true; //< Not a fatal error.
    }
    const auto& buffer = data->getBuffer();
    // The relay has no consent timer; only a fatal inner-packet result tears it down.
    return processIncomingPacket((const uint8_t*) buffer.data(), buffer.size())
        != PacketResult::fatal;
}

bool IceRelay::processRelayPackets()
{
    while (!m_readBuffer.empty())
    {
        stun::Message message;
        stun::MessageParser stunParser;
        stunParser.setMessage(&message);
        size_t parsed = 0;

        if (stunParser.parse(m_readBuffer, &parsed) != nx::network::server::ParserState::done)
        {
            NX_DEBUG(this, "TURN message not parsed for %1", m_sessionId);
            return true; //< Not a fatal error.
        }

        switch (m_relayStage)
        {
            case RelayStage::Binding:
            {
                if (!handleBinding(message))
                    return false;
                break;
            }
            case RelayStage::AllocateNoAuth:
            {
                if (!handleAllocateNoAuth(message))
                    return false;
                break;
            }
            case RelayStage::Allocate:
            {
                if (!handleAllocate(message))
                    return false;
                break;
            }
            case RelayStage::CreatePermission:
            {
                if (!handleCreatePermission(message))
                    return false;
                break;
            }
            case RelayStage::Indication:
            {
                if (!handleIndication(message))
                    return false;
                break;
            }
            default:
            {
                NX_ASSERT(
                    false,
                    "Unexpected state: %1 for %2",
                    static_cast<int>(m_relayStage),
                    m_sessionId);
                return false;
            }
        }

        m_readBuffer.erase(0, parsed);
    }
    return true;
}

void IceRelay::writeRelayPacket(const char* data, int size)
{
    NX_ASSERT(m_pollable.isInSelfAioThread());
    // Always foreground.
    insertForegroundBuffer(nx::Buffer(data, (size_t) size));

    sendNextBufferUnsafe();
}

void IceRelay::onBytesRead(SystemError::ErrorCode errorCode, std::size_t bytesTransferred)
{
    if (m_needStop)
        return;

    if (nx::network::socketCannotRecoverFromError(errorCode) || bytesTransferred == 0)
    {
        stopUnsafe();
    }
    else
    {
        bool result = processRelayPackets() && m_socket;
        if (result)
        {
            m_keepAliveTimer.start(
                kKeepAliveTimeout,
                [this]()
                {
                    NX_DEBUG(
                        this,
                        "Keep alive timeout reached, stopping Relay ice for %1",
                        m_sessionId);
                    stopUnsafe(); //< Ice restart is better.
                });
            m_socket->readSomeAsync(
                &m_readBuffer,
                [this](auto... args)
                {
                    onBytesRead(std::move(args)...);
                });
        }
        else
        {
            stopUnsafe();
        }
    }
}

void IceRelay::asyncSendPacketUnsafe()
{
    // See IceUdp::asyncSendPacketUnsafe: same thread, so no trip through the reactor.
    if (NX_ASSERT(m_socket))
    {
        m_socket->sendAsync(
            &m_sendingBuffer,
            [this](auto... args)
            {
                onBytesWritten(std::move(args)...);
            });
    }
}

nx::network::SocketAddress IceRelay::iceRemoteAddress() const
{
    if (NX_ASSERT(m_socket))
        return m_socket->getForeignAddress();

    return {};
}

nx::network::SocketAddress IceRelay::iceLocalAddress() const
{
    if (NX_ASSERT(m_socket))
        return m_socket->getLocalAddress();

    return {};
}

} // namespace nx::webrtc
