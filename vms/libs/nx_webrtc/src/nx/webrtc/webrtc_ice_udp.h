// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <atomic>

#include "webrtc_ice.h"

namespace nx::webrtc {

class NX_WEBRTC_API IceUdp: public Ice
{
public:
    IceUdp(
        SessionPool* sessionPool,
        const SessionConfig& config,
        aio::AbstractAioThread* aioThread = nullptr);
    virtual ~IceUdp();
    std::optional<nx::network::SocketAddress> bind(const nx::network::HostAddress& host);
    void resetSocket(std::unique_ptr<nx::network::AbstractDatagramSocket>&& socket);
    void sendBinding(
        const IceCandidate& iceCandidate,
        const SessionDescription& sessionDescription);
    virtual IceCandidate::Filter type() const override;

    /** True when bind() attached this ice to an IPv6 local address. */
    bool isBoundToV6() const { return m_isV6; }

    /**
     * True once this ice has a punch target (latched from a tracker candidate or
     * learned from an authenticated peer packet). The binding timer chain is
     * self-perpetuating from that point, so feeding further tracker candidates to
     * sendBinding would be a no-op - the session skips such ices.
     */
    bool isPunching() const { return m_punching; }

    /** Routing scope of an IPv6 address; a punch is only viable within one scope. */
    enum class IpV6Scope
    {
        loopback,
        linkLocal,
        uniqueLocal,
        global,
    };

protected:
    // From Ice.
    virtual nx::network::SocketAddress iceRemoteAddress() const override final;
    virtual nx::network::SocketAddress iceLocalAddress() const override final;
    virtual void stopUnsafe() override final;
    virtual void asyncSendPacketUnsafe() override final;
    virtual void onPeerValidated(const nx::network::SocketAddress& peer) override final;

private:
    void onBytesRead(SystemError::ErrorCode errorCode, std::size_t bytesTransferred);

    /**
     * Whether this socket can plausibly route a punch to target: the address family
     * must match, and for IPv6 the routing scope must match too. A ULA-bound socket
     * (e.g. Tailscale) has no route to a global target, and the resulting send error
     * would fatally stop this ice, destroying a path that could still validate
     * passively. Link-local is never punched - the SDP candidate carries no scope id.
     */
    bool canPunchTo(const nx::network::HostAddress& target) const;

private:
    nx::Buffer m_readBuffer;
    std::unique_ptr<nx::network::AbstractDatagramSocket> m_socket;
    /// How this ice was created: UdpHost for a bind()-created host socket,
    /// UdpSrflx for the srflx-punched socket adopted via resetSocket(). Host
    /// ices keep reporting UdpHost even after they punch (the IPv6 pinhole),
    /// so metrics count them as direct host connections - do not infer the
    /// type from m_bindingCount.
    IceCandidate::Filter m_type = IceCandidate::Filter::UdpHost;
    bool m_firstPacket = true;
    /// Address family of the bound socket; a v4 socket cannot punch a v6
    /// target and vice versa (see the family guard in sendBinding).
    bool m_isV6 = false;
    /// Routing scope of the bound address; meaningful only when m_isV6 is true.
    IpV6Scope m_ipV6Scope = IpV6Scope::global;
    nx::network::aio::Timer m_keepAliveTimer;
    nx::network::aio::Timer m_bindingTimer;
    int m_bindingCount = 0;
    /// Address our connectivity checks are sent to. Seeded from the advertised
    /// remote candidate, then retargeted to the authenticated peer-reflexive
    /// source learned via onPeerValidated().
    nx::network::SocketAddress m_punchTarget;
    /// Set on the aio thread whenever m_punchTarget becomes non-null; read by the
    /// session thread (isPunching) to avoid dispatching redundant punches.
    std::atomic<bool> m_punching{false};
};

} // namespace nx::webrtc
