// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <QtCore/QString>

#include <nx/network/socket_common.h>
#include <nx/network/system_socket.h>
#include <nx/utils/buffer.h>
#include <nx/utils/thread/mutex.h>
#include <nx/utils/timer_manager.h>

namespace nx::network::upnp {

class DeviceSearcher;
class PortMapper;

} // namespace nx::network::upnp

namespace nx::webrtc {

class SessionPool;
class IceUdpMuxed;

/**
 * The id of the session whose peer sent packet: a STUN binding request whose USERNAME
 * "<local ufrag>:<remote ufrag>" names the session by its left part, and whose
 * MESSAGE-INTEGRITY verifies with the ice-pwd of that session. Null for any other datagram.
 * @param localPwdOf The ice-pwd of a session id, or null when there is no such session.
 */
NX_WEBRTC_API std::optional<std::string> authenticatedSessionId(
    const nx::Buffer& packet,
    const std::function<std::optional<std::string>(const std::string& sessionId)>& localPwdOf);

/**
 * One UDP socket on a fixed port that carries the ICE of every session, so that one router port
 * mapping reaches all of them. The mapping is made here through UPnP. A datagram goes to the ice
 * of its source address. A new source address must first send a binding request that
 * authenticates for a live session (see authenticatedSessionId()): then it either gets a new ice
 * on the session's aio thread, or, when the session's path on this port has gone silent, takes
 * that path over, as when the peer's NAT or network changes its address.
 */
class UdpMux
{
public:
    /** The sessionPool outlives the mux: the pool owns it. */
    UdpMux(SessionPool* sessionPool, nx::utils::TimerManager* timerManager);
    ~UdpMux();

    /**
     * Binds port on all IPv4 interfaces and, if upnpEnabled, keeps it mapped on the router.
     * @param mappingDescription The name of the mapping in the router's table.
     * @return False if the port cannot be bound.
     */
    bool start(uint16_t port, bool upnpEnabled, const QString& mappingDescription);

    /** Stops the port mapping and the reading, and destroys the ices on their own threads. */
    void stop();

    /** The local port, valid after a successful start(). */
    uint16_t port() const;

    /** The public address of the port on the router, while a UPnP mapping holds it. */
    std::optional<nx::network::SocketAddress> mappedAddress() const;

    /**
     * Sends a datagram to peer from the shared port. Callable from any thread.
     * @return False if the datagram was not sent, for example on a full send buffer.
     */
    bool send(const nx::network::SocketAddress& peer, const nx::Buffer& data);

    /** Destroys the ices that have stopped, each on its own aio thread. */
    void removeStoppedIces();

private:
    void readNext();
    void onBytesRead(SystemError::ErrorCode errorCode, std::size_t bytesTransferred);
    void routePacket(const nx::network::SocketAddress& source, const nx::Buffer& packet);
    void routeFromNewSource(const nx::network::SocketAddress& source, const nx::Buffer& packet);
    void onMappingChanged(const nx::network::SocketAddress& externalAddress);

private:
    SessionPool* const m_sessionPool;
    nx::utils::TimerManager* const m_timerManager;

    std::unique_ptr<nx::network::UDPSocket> m_socket;
    nx::Buffer m_readBuffer;
    uint16_t m_port = 0;

    /** UDPSocket::sendTo() stores the destination in the socket, so one send at a time. */
    nx::Mutex m_sendMutex;

    mutable nx::Mutex m_mutex;
    std::map<std::string, std::unique_ptr<IceUdpMuxed>> m_iceBySource;
    std::optional<nx::network::SocketAddress> m_mappedAddress;

    std::unique_ptr<nx::network::upnp::DeviceSearcher> m_deviceSearcher;
    std::unique_ptr<nx::network::upnp::PortMapper> m_portMapper;
};

} // namespace nx::webrtc
