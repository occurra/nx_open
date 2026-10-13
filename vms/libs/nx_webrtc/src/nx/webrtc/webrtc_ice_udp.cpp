// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "webrtc_ice_udp.h"

#include <cstring>

#include <nx/network/socket_factory.h>
#include <nx/network/system_socket.h>
#include <nx/utils/log/log.h>

namespace nx::webrtc {

constexpr size_t kRawReadBufferSize = nx::network::AbstractDatagramSocket::MAX_DATAGRAM_SIZE;
constexpr std::chrono::seconds kKeepAliveTimeout{15};
constexpr std::chrono::milliseconds kBindingTimeout{50};
constexpr int kBindingCountLimit = 100; // Rapid-punch for first 5 seconds.
constexpr std::chrono::seconds kKeepAliveInterval{2}; // NAT keepalive after connectivity.

using namespace nx::network;

namespace {

IceUdp::IpV6Scope ipV6ScopeOf(const HostAddress& host)
{
    // NOTE: HostAddress::isLocalNetwork() checks `fd00:*` literally and misses the
    // rest of fc00::/7 (e.g. Tailscale fd7a:...), so classify from the raw bytes.
    if (const auto ip = host.ipV6().first) //< No `&` to work around an MSVC bug.
    {
        if (std::memcmp(&*ip, &in6addr_loopback, sizeof(*ip)) == 0)
            return IceUdp::IpV6Scope::loopback;
        if (ip->s6_addr[0] == 0xFE && (ip->s6_addr[1] & 0xC0) == 0x80) //< fe80::/10.
            return IceUdp::IpV6Scope::linkLocal;
        if ((ip->s6_addr[0] & 0xFE) == 0xFC) //< fc00::/7, unique local.
            return IceUdp::IpV6Scope::uniqueLocal;
    }
    return IceUdp::IpV6Scope::global;
}

// True when both are IPv6 and share the same /64 prefix (the first 8 bytes).
// IPv6 privacy/temporary addresses rotate the low 64 bits (the interface id)
// while this prefix and the socket's port stay fixed, so a same-/64 source is
// the same host on the same socket.
bool sameIpv6Prefix64(const HostAddress& a, const HostAddress& b)
{
    const auto a6 = a.ipV6().first; //< No `&`, per the MSVC note above.
    const auto b6 = b.ipV6().first;
    if (!a6 || !b6)
        return false;
    return std::memcmp(a6->s6_addr, b6->s6_addr, 8) == 0; //< First 64 bits = the /64.
}

} // namespace

IceUdp::IceUdp(
    SessionPool* sessionPool,
    const SessionConfig& config,
    aio::AbstractAioThread* aioThread):
    Ice(sessionPool, config, aioThread)
{
    m_readBuffer.reserve(kRawReadBufferSize);
    m_keepAliveTimer.bindToAioThread(m_pollable.getAioThread());
    m_bindingTimer.bindToAioThread(m_pollable.getAioThread());
}

IceUdp::~IceUdp()
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

void IceUdp::stopUnsafe()
{
    if (!m_needStop)
    {
        m_keepAliveTimer.cancelSync();
        m_bindingTimer.cancelSync();
        m_socket->cancelIOSync(aio::EventType::etAll);

        Ice::stopUnsafe(); //< Dangerous, object can be destroyed after this call.
    }
}

IceCandidate::Filter IceUdp::type() const
{
    return m_type;
}

void IceUdp::resetSocket(std::unique_ptr<nx::network::AbstractDatagramSocket>&& socket)
{
    m_type = IceCandidate::Filter::UdpSrflx;
    m_socket = std::move(socket);
    NX_VERBOSE(this, "Running ice: srflx to %1", iceLocalAddress().toString());

    m_socket->bindToAioThread(m_pollable.getAioThread());

    m_socket->readSomeAsync(
        &m_readBuffer,
        [this](auto... args)
        {
            onBytesRead(std::move(args)...);
        });
}

std::optional<nx::network::SocketAddress> IceUdp::bind(const nx::network::HostAddress& host)
{
    // Match the socket's address family to the host address. The factory
    // default is IPv4-only, which made every IPv6 bind fail silently and
    // stripped IPv6 UDP host candidates from the offer.
    m_isV6 = host.isPureIpV6();
    if (m_isV6)
        m_ipV6Scope = ipV6ScopeOf(host);
    m_socket = std::make_unique<nx::network::UDPSocket>(m_isV6 ? AF_INET6 : AF_INET);
    m_socket->bindToAioThread(m_pollable.getAioThread());
    m_socket->setNonBlockingMode(true);
    if (!m_socket->bind(nx::network::SocketAddress(host, 0)))
        return std::nullopt;

    NX_VERBOSE(this, "Running ice: binded to %1", iceLocalAddress().toString());

    m_socket->readSomeAsync(
        &m_readBuffer,
        [this](auto... args)
        {
            onBytesRead(std::move(args)...);
        });
    return iceLocalAddress();
}

void IceUdp::sendBinding(
    const IceCandidate& iceCandidate,
    const SessionDescription& sessionDescription)
{
    NX_CRITICAL(m_socket);
    m_socket->dispatch(
        [this, iceCandidate, sessionDescription]
        {
            if (m_needStop)
                return;

            // Remember the session id: a binding success response to this
            // check carries no USERNAME, so handleStun can attribute it to the
            // session (onDirectPathProbeSucceeded) only through this field.
            // Deliberately not m_sessionDescription - that would bypass the
            // locked/expired gate on the binding request path. Same aio thread
            // as handleStun - no locking needed.
            m_punchSessionId = sessionDescription.id;

            // Before connectivity is established, punch rapidly and give up after
            // kBindingCountLimit if nothing connects. Once the STUN handshake has
            // moved us past the binding stage, keep sending at a slow cadence as a
            // NAT keepalive so the pinhole stays open through the DTLS handshake and
            // for the session's lifetime — otherwise a restrictive NAT closes the
            // inbound hole before the peer's ClientHello arrives.
            const bool punching = (m_stage == Stage::binding);
            if (punching && ++m_bindingCount > kBindingCountLimit)
                return;

            // Punch toward the peer-reflexive source once it has been learned
            // (onPeerValidated); until then, target the advertised candidate.
            if (m_punchTarget.isNull())
            {
                // Skip targets this socket cannot route to (family or, for IPv6,
                // scope mismatch) and let the ice bound to a matching address do
                // the punching. See canPunchTo().
                if (iceCandidate.address.isNull() || !canPunchTo(iceCandidate.address.address))
                {
                    NX_DEBUG(
                        this,
                        "STUN: %1: Cannot punch from %2 to %3 (family or scope mismatch), ignoring",
                        sessionDescription.id,
                        iceLocalAddress(),
                        iceCandidate.address);
                    return;
                }
                m_punchTarget = iceCandidate.address;
                m_punching = true;
            }

            m_firstPacket = false;
            m_socket->setDestAddr(m_punchTarget);

            // Controlled-role full-ICE connectivity check: MESSAGE-INTEGRITY is keyed
            // with the peer's ice-pwd (so the peer accepts it) and the request carries
            // ICE-CONTROLLED with our tie-breaker. See buildControlledBindingRequest().
            nx::Buffer sendBuffer = buildControlledBindingRequest(
                sessionDescription.remoteUfrag,
                sessionDescription.id,
                sessionDescription.remotePwd,
                iceCandidate.priority,
                sessionDescription.tieBreaker);
            writePacket(sendBuffer.data(), sendBuffer.size(), /*foreground*/ true);

            NX_VERBOSE(this, "STUN: binding sent for session %1", sessionDescription.id);

            m_bindingTimer.start(
                punching ? kBindingTimeout : kKeepAliveInterval,
                [this, iceCandidate, sessionDescription]()
                {
                    sendBinding(iceCandidate, sessionDescription);
                });
        });
}

bool IceUdp::canPunchTo(const nx::network::HostAddress& target) const
{
    if (target.isPureIpV6() != m_isV6)
        return false;
    if (!m_isV6)
        return true;

    const IpV6Scope targetScope = ipV6ScopeOf(target);
    return targetScope == m_ipV6Scope && targetScope != IpV6Scope::linkLocal;
}

void IceUdp::onBytesRead(SystemError::ErrorCode errorCode, std::size_t bytesTransferred)
{
    if (m_needStop)
        return;

    if (nx::network::socketCannotRecoverFromError(errorCode) || bytesTransferred == 0)
    {
        stopUnsafe();
    }
    else
    {
        if (m_readBuffer.size())
        {
            if (m_firstPacket)
            {
                // NOTE: for host candidate, we can receive packet only from remote host candidate.
                // For srflx candidate, we can receive packet from remote srflx or from STUN
                // server. We set right address for srflx in sendBinding().
                m_socket->setDestAddr(m_socket->lastDatagramSourceAddress());
            }

            const PacketResult result =
                processIncomingPacket((uint8_t*) m_readBuffer.data(), m_readBuffer.size());
            if (m_needStop) // processIncomingPacket can call stopUnsafe on write data.
                return;
            m_readBuffer.resize(0);
            if (result == PacketResult::fatal)
            {
                stopUnsafe();
                return;
            }

            // Any non-fatal packet means a source is latched; stop the first-packet probe.
            m_firstPacket = false;

            // Only authenticated checks / own-protocol data refresh the receive timeout.
            // Dropped foreign packets (port scans, stray STUN) must not keep a dead ice
            // alive past kKeepAliveTimeout.
            if (result == PacketResult::valid)
            {
                m_keepAliveTimer.start(
                    kKeepAliveTimeout,
                    [this]()
                    {
                        NX_DEBUG(
                            this,
                            "Keep alive timeout reached, stopping UDP ice for %1",
                            (m_sessionDescription ? m_sessionDescription->id : "(null)"));
                        stopUnsafe(); //< Ice restart is better.
                    });
            }
        }

        m_socket->readSomeAsync(
            &m_readBuffer,
            [this](auto... args)
            {
                onBytesRead(std::move(args)...);
            });
    }
};

void IceUdp::asyncSendPacketUnsafe()
{
    // The socket shares this ice's aio thread and every caller is already on it, so the send
    // starts here instead of costing a trip through the reactor. sendAsync only arms write
    // monitoring; the completion arrives later from the poll loop, and re-arming from inside
    // that completion is what the helper's send counter exists for.
    m_socket->sendAsync(
        &m_sendingBuffer,
        [this](auto... args)
        {
            onBytesWritten(std::move(args)...);
        });
}

nx::network::SocketAddress IceUdp::iceRemoteAddress() const
{
    return m_socket->lastDatagramSourceAddress();
}

nx::network::SocketAddress IceUdp::iceLocalAddress() const
{
    return m_socket->getLocalAddress();
}

void IceUdp::onPeerValidated(const nx::network::SocketAddress& peer)
{
    // An authenticated STUN packet arrived from `peer`. If that differs from
    // where we are currently sending (the advertised candidate, or a previously
    // learned peer-reflexive address), retarget both our STUN responses and our
    // connectivity checks to it. Runs on the socket's aio thread (handleStun is
    // reached via onBytesRead), the same thread sendBinding's timer runs on, so
    // updating m_punchTarget here needs no extra locking.
    // Compare by string form: m_punchTarget may hold the address parsed from SDP
    // (string) while `peer` comes from recvfrom (binary), and SocketAddress::operator==
    // treats those as different even for the same endpoint.
    if (peer.isNull() || peer.toString() == m_punchTarget.toString())
        return;

    // A same-port, same-/64 source is the same host cycling an IPv6 privacy
    // address; retargeting to it thrashes the transport. A real remote move
    // changes the port (NAT rebind) or the /64, so this keeps following those.
    if (peer.port == m_punchTarget.port && sameIpv6Prefix64(peer.address, m_punchTarget.address))
    {
        return;
    }

    NX_DEBUG(
        this,
        "STUN: learned peer-reflexive address %1 (was %2), retargeting",
        peer,
        m_punchTarget);
    m_punchTarget = peer;
    m_punching = true;
    m_socket->setDestAddr(peer);
}

} // namespace nx::webrtc
