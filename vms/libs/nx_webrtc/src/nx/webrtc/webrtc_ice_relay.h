// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <memory>

#include <nx/network/abstract_socket.h>

#include "webrtc_ice.h"

namespace nx::webrtc {

class NX_WEBRTC_API IceRelay: public Ice
{
public:
    using CandidateHandler = std::function<void(const IceCandidate&)>;

public:
    IceRelay(
        SessionPool* sessionPool,
        const SessionConfig& config,
        const std::string& sessionId,
        const CandidateHandler& handler,
        aio::AbstractAioThread* aioThread = nullptr);
    virtual ~IceRelay();
    /**
     * Registers a peer address the TURN permission must cover. Accepts the
     * peer's srflx candidate (the classic case) and its relay candidate: a
     * CGNAT peer reaches distinct destinations from distinct public IPs, so
     * the srflx it signals may never be the address our TURN server sees its
     * packets from - but its relayed address (the TURN server's own IP) always
     * is. All registered IPs go into one CreatePermission request.
     */
    void addRemotePeer(const IceCandidate& candidate);
    void startRelay(const TurnServerInfo& turnServer);
    virtual IceCandidate::Filter type() const override;

protected:
    // From Ice.
    virtual nx::network::SocketAddress iceRemoteAddress() const override final;
    virtual nx::network::SocketAddress iceLocalAddress() const override final;
    virtual void stopUnsafe() override final;
    virtual void asyncSendPacketUnsafe() override final;
    virtual nx::Buffer toSendBuffer(const char* data, int dataSize) const override final;

private:
    void writeRelayPacket(const char* data, int size);
    void onBytesRead(SystemError::ErrorCode errorCode, std::size_t bytesTransferred);
    bool processRelayPackets();
    void bindToRelay();
    void sendAllocate();
    void sendRefresh();
    void scheduleRefresh();
    void createPermission();
    void fillAuthorizationAndIntegrity(stun::Message& message);
    bool handleBinding(const stun::Message& message);
    bool handleAllocateNoAuth(const stun::Message& message);
    bool handleAllocate(const stun::Message& message);
    bool handleCreatePermission(const stun::Message& message);
    bool handleIndication(const stun::Message& message);
    bool handleLongTermAuthorization(const stun::Message& message);
    bool handleRefresh(const stun::Message& message);
    void connectCurrentEndpoint();

private:
    enum RelayStage
    {
        Binding,
        AllocateNoAuth,
        Allocate,
        CreatePermission,
        Indication,
        Unknown
    };
    enum AuthorizationType
    {
        None,
        LongTerm
    };

private:
    nx::Buffer m_readBuffer;

    std::unique_ptr<nx::network::AbstractCommunicatingSocket> m_socket;
    size_t m_endpointIndex = 0; //< Index into m_turnServer.endpoints for the current attempt.

    TurnServerInfo m_turnServer;
    std::string m_nonce;
    std::string m_realm;
    std::string m_password;
    AuthorizationType m_turnAuth = AuthorizationType::None;
    const std::string m_sessionId;
    RelayStage m_relayStage = RelayStage::Binding;
    std::optional<IceCandidate> m_remoteSrflx; //< Preferred send target until the peer speaks.
    /**
     * Every peer address a permission must be installed for, deduplicated by
     * IP (permissions are per-IP, RFC 5766). Holds srflx and relay candidate
     * addresses; m_remoteSrflx is always among them when known.
     */
    std::vector<nx::network::SocketAddress> m_permittedPeers;
    std::optional<nx::network::SocketAddress> m_peerAddress;
    std::optional<IceCandidate> m_localRelay;
    nx::network::aio::Timer m_keepAliveTimer;
    nx::network::aio::Timer m_createPermissionTimer;
    nx::network::aio::Timer m_refreshTimer;
    int m_refreshLifetimeSeconds = 600;
    CandidateHandler m_onCandidateReadyHandler;
};

} // namespace nx::webrtc
