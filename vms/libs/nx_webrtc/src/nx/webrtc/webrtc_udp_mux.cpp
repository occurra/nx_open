// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "webrtc_udp_mux.h"

#include <algorithm>
#include <atomic>
#include <future>
#include <set>
#include <vector>

#include <nx/network/stun/message.h>
#include <nx/network/stun/message_parser.h>
#include <nx/network/upnp/upnp_device_searcher.h>
#include <nx/network/upnp/upnp_port_mapper.h>
#include <nx/utils/elapsed_timer.h>
#include <nx/utils/log/log.h>

#include "webrtc_ice.h"
#include "webrtc_session.h"
#include "webrtc_session_pool.h"

namespace nx::webrtc {

using namespace nx::network;
using namespace std::chrono;

namespace {

constexpr size_t kReadBufferSize = AbstractDatagramSocket::MAX_DATAGRAM_SIZE;

/** Room for the media of many sessions on one socket. */
constexpr unsigned int kSocketBufferSize = 4 * 1024 * 1024;

/** Bounds the ices that datagrams from unknown sources can create. */
constexpr size_t kMaxIceCount = 1024;

/** An ice whose peer sends nothing valid for this long stops, as IceUdp does. */
constexpr auto kKeepAliveTimeout = 15s;

/**
 * A session's path must be silent this long before a new source address of its peer takes it
 * over. Shorter than kKeepAliveTimeout, and longer than the gaps of a live path, which carries
 * RTCP and SCTP acknowledgements every second or more often.
 */
constexpr auto kPeerMoveSilence = 3s;

/** The UPnP device type of a router that maps ports. */
const QString kInternetGatewayDeviceType("InternetGatewayDevice");

/** Any public address: a connected UDP socket only asks the system for the route to it. */
const SocketAddress kInternetProbeAddress(HostAddress("8.8.8.8"), 53);

int64_t steadyNowMs()
{
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/** The local IPv4 address that the system sends internet traffic from; empty without a route. */
std::string probeInternetInterfaceAddress()
{
    UDPSocket socket(AF_INET);
    if (!socket.connect(kInternetProbeAddress, kNoTimeout))
        return std::string();
    return socket.getLocalAddress().address.toString();
}

/**
 * Maps only on the routers found through the interface that leads to the internet. A UPnP router
 * of another network, for example of the cameras, gives no address that a peer can reach, and its
 * mapping would replace the one of the internet router.
 */
class InternetPortMapper: public upnp::PortMapper
{
public:
    using upnp::PortMapper::PortMapper;

protected:
    virtual bool processPacket(
        const QHostAddress& localAddress,
        const SocketAddress& devAddress,
        const upnp::DeviceInfo& devInfo,
        const QByteArray& xmlDevInfo) override
    {
        const std::string internetAddress = this->internetAddress();
        if (!internetAddress.empty() && internetAddress == localAddress.toString().toStdString())
            return upnp::PortMapper::processPacket(localAddress, devAddress, devInfo, xmlDevInfo);

        {
            NX_MUTEX_LOCKER lock(&m_internetMutex);
            if (!m_ignored.insert(devAddress.toString()).second)
                return false; //< Each search finds it again.
        }
        NX_DEBUG(
            this,
            "Ignoring the UPnP router %1 found through %2: the internet goes through %3",
            devAddress,
            localAddress.toString(),
            internetAddress.empty() ? std::string("no interface") : internetAddress);
        return false;
    }

private:
    /** Probes the route again at most once per kDefaultCheckMappingsInterval. */
    std::string internetAddress()
    {
        {
            NX_MUTEX_LOCKER lock(&m_internetMutex);
            if (!m_internetChecked.hasExpired(kDefaultCheckMappingsInterval))
                return m_internetAddress;
        }

        const std::string address = probeInternetInterfaceAddress();
        NX_MUTEX_LOCKER lock(&m_internetMutex);
        m_internetAddress = address;
        m_internetChecked.restart();
        return address;
    }

private:
    nx::Mutex m_internetMutex;
    std::string m_internetAddress;
    nx::utils::ElapsedTimer m_internetChecked;
    std::set<std::string> m_ignored;
};

} // namespace

std::optional<std::string> authenticatedSessionId(
    const nx::Buffer& packet,
    const std::function<std::optional<std::string>(const std::string& sessionId)>& localPwdOf)
{
    stun::Message message;
    stun::MessageParser parser;
    parser.setFingerprintRequired(true);
    parser.setMessage(&message);
    size_t parsed = 0;
    if (parser.parse(packet, &parsed) != nx::network::server::ParserState::done)
        return std::nullopt;

    if (message.header.messageClass != stun::MessageClass::request
        || message.header.method != stun::MethodType::bindingMethod)
    {
        return std::nullopt;
    }

    const auto username = message.getAttribute<stun::attrs::UserName>();
    if (!username)
        return std::nullopt;

    const std::string value = username->getString();
    const auto colonPos = value.find(':');
    if (colonPos == std::string::npos)
        return std::nullopt;

    const std::string sessionId = value.substr(0, colonPos);
    const auto localPwd = localPwdOf(sessionId);
    if (!localPwd || !message.verifyIntegrity(value, *localPwd))
        return std::nullopt;
    return sessionId;
}

//-------------------------------------------------------------------------------------------------

/**
 * The ice of one peer address on the shared UDP port. It has no socket of its own: UdpMux hands
 * it the datagrams of its peer, and it sends through UdpMux. It runs on its session's aio thread.
 */
class IceUdpMuxed: public Ice
{
public:
    /** The mux owns this ice, so it outlives it. */
    IceUdpMuxed(
        SessionPool* sessionPool,
        const SessionConfig& config,
        aio::AbstractAioThread* aioThread,
        UdpMux* mux,
        const std::string& sessionId,
        const SocketAddress& peer,
        const SocketAddress& localAddress):
        Ice(sessionPool, config, aioThread),
        m_mux(mux),
        m_sessionId(sessionId),
        m_peer(peer),
        m_localAddress(localAddress),
        m_lastValidAtMs(steadyNowMs())
    {
        m_keepAliveTimer.bindToAioThread(m_pollable.getAioThread());
    }

    virtual ~IceUdpMuxed() override
    {
        std::promise<void> stopped;
        m_pollable.dispatch(
            [this, &stopped]()
            {
                stopUnsafe();
                stopped.set_value();
            });
        stopped.get_future().wait();
    }

    /** Processes a datagram of the peer on this ice's thread. Callable from any thread. */
    void deliver(nx::Buffer packet)
    {
        m_pollable.post(
            [this, packet = std::move(packet)]()
            {
                onPacket(packet);
            });
    }

    /**
     * Moves this ice to a new address of its peer. Callable from any thread; the packets
     * delivered after the call already see the new address.
     */
    void movePeer(const SocketAddress& peer)
    {
        m_pollable.post(
            [this, peer]()
            {
                NX_DEBUG(
                    this,
                    "Peer of session %1 moved from %2 to %3",
                    m_sessionId,
                    m_peer,
                    peer);
                m_peer = peer;
            });
    }

    bool isStopped() const { return m_stopped; }

    const std::string& sessionId() const { return m_sessionId; }

    /** Whether this ice is the path of its session: DTLS is done and the session is its own. */
    bool holdsSession() const { return m_holdsSession; }

    /** Whether no authenticated packet came from the peer for this long. */
    bool isSilentFor(milliseconds duration) const
    {
        return steadyNowMs() - m_lastValidAtMs > duration.count();
    }

    virtual IceCandidate::Filter type() const override
    {
        // A peer on the local network reaches the port directly, any other through the mapping.
        return isPrivateScope(m_peer.address) ? IceCandidate::Filter::UdpHost
                                              : IceCandidate::Filter::UdpMapped;
    }

protected:
    virtual SocketAddress iceRemoteAddress() const override { return m_peer; }
    virtual SocketAddress iceLocalAddress() const override { return m_localAddress; }

    virtual void stopUnsafe() override
    {
        if (m_needStop)
            return;

        m_keepAliveTimer.cancelSync();
        m_stopped = true;
        Ice::stopUnsafe();
    }

    virtual void asyncSendPacketUnsafe() override
    {
        // A datagram the socket cannot take is lost, as on a lossy link; SRTP and SCTP recover.
        m_mux->send(m_peer, m_sendingBuffer);
        m_pollable.post(
            [this]()
            {
                onBytesWritten(SystemError::noError, m_sendingBuffer.size());
            });
    }

private:
    void onPacket(const nx::Buffer& packet)
    {
        if (m_needStop)
            return;

        const PacketResult result =
            processIncomingPacket((const uint8_t*) packet.data(), (int) packet.size());
        if (m_needStop) //< processIncomingPacket() can stop this ice.
            return;
        if (result == PacketResult::fatal)
        {
            stopUnsafe();
            return;
        }

        if (result == PacketResult::valid)
        {
            m_lastValidAtMs = steadyNowMs();
            m_holdsSession = m_stage == Stage::streaming;
        }

        // Only authenticated checks and own-protocol data keep the ice alive, so a source that
        // never authenticates stops too.
        if (result == PacketResult::valid || !m_keepAliveTimer.timeToEvent())
            restartKeepAliveTimer();
    }

    void restartKeepAliveTimer()
    {
        m_keepAliveTimer.start(
            kKeepAliveTimeout,
            [this]()
            {
                NX_DEBUG(
                    this,
                    "Keep alive timeout reached, stopping the shared-port ice of %1 for %2",
                    m_peer,
                    m_sessionId);
                stopUnsafe();
            });
    }

private:
    UdpMux* const m_mux;
    const std::string m_sessionId;

    /** Changed by movePeer(); read and written on this ice's thread. */
    SocketAddress m_peer;

    const SocketAddress m_localAddress;
    nx::network::aio::Timer m_keepAliveTimer;
    std::atomic<bool> m_stopped = false;

    /** Read by the mux thread; written on this ice's thread. */
    std::atomic<bool> m_holdsSession = false;
    std::atomic<int64_t> m_lastValidAtMs;
};

namespace {

/** Ices must die on their own aio thread; see the note on destroyOnOwnAioThread in the pool. */
std::future<void> destroyOnOwnAioThread(std::unique_ptr<IceUdpMuxed> ice)
{
    auto done = std::make_shared<std::promise<void>>();
    auto result = done->get_future();
    auto* const aioThread = ice->getAioThread();
    aioThread->post(
        nullptr,
        [ice = std::move(ice), done]() mutable
        {
            ice.reset();
            done->set_value();
        });
    return result;
}

} // namespace

//-------------------------------------------------------------------------------------------------

UdpMux::UdpMux(SessionPool* sessionPool, nx::utils::TimerManager* timerManager):
    m_sessionPool(sessionPool),
    m_timerManager(timerManager)
{
}

UdpMux::~UdpMux()
{
    stop();
}

bool UdpMux::start(uint16_t port, bool upnpEnabled, const QString& mappingDescription)
{
    auto socket = std::make_unique<UDPSocket>(AF_INET);
    if (!socket->setNonBlockingMode(true)
        || !socket->bind(SocketAddress(HostAddress::anyHost, port)))
    {
        NX_WARNING(
            this,
            "Cannot bind the shared WebRTC UDP port %1: %2. Free the port or configure another "
            "one; until then a peer that srflx cannot reach uses the TURN relay",
            port,
            SystemError::getLastOSErrorText());
        return false;
    }
    if (!socket->setSendBufferSize(kSocketBufferSize)
        || !socket->setRecvBufferSize(kSocketBufferSize))
    {
        NX_DEBUG(
            this,
            "Cannot enlarge the buffers of the shared WebRTC UDP port %1: %2",
            port,
            SystemError::getLastOSErrorText());
    }

    m_port = socket->getLocalAddress().port;
    m_socket = std::move(socket);
    m_readBuffer.reserve(kReadBufferSize);
    m_socket->dispatch(
        [this]()
        {
            readNext();
        });
    NX_INFO(this, "Serving WebRTC on the shared UDP port %1", m_port);

    if (upnpEnabled)
    {
        m_deviceSearcher = std::make_unique<upnp::DeviceSearcher>(
            m_timerManager,
            std::make_unique<upnp::DeviceSearcherDefaultSettings>());
        m_portMapper = std::make_unique<InternetPortMapper>(
            m_timerManager,
            /*isEnabled*/ true,
            upnp::PortMapper::kDefaultCheckMappingsInterval,
            mappingDescription);
        m_deviceSearcher->registerHandler(m_portMapper.get(), kInternetGatewayDeviceType);
        m_deviceSearcher->start();
        m_portMapper->enableMapping(
            m_port,
            upnp::PortMapper::Protocol::udp,
            [this](SocketAddress externalAddress)
            {
                onMappingChanged(externalAddress);
            });
        NX_INFO(
            this,
            "Looking for a UPnP router to map the shared WebRTC UDP port %1; until one maps it, "
            "a peer that srflx cannot reach uses the TURN relay",
            m_port);
    }
    return true;
}

void UdpMux::stop()
{
    if (m_portMapper)
    {
        // The router drops a mapping that this cannot delete in time when its lease runs out.
        m_portMapper->disableMapping(m_port, upnp::PortMapper::Protocol::udp);
        m_deviceSearcher->unregisterHandler(m_portMapper.get());
        m_portMapper.reset();
    }
    m_deviceSearcher.reset();

    if (m_socket)
        m_socket->pleaseStopSync();

    decltype(m_iceBySource) ices;
    {
        NX_MUTEX_LOCKER lock(&m_mutex);
        std::swap(ices, m_iceBySource);
    }

    std::vector<std::future<void>> destroyed;
    for (auto& [source, ice]: ices)
        destroyed.push_back(destroyOnOwnAioThread(std::move(ice)));
    for (auto& future: destroyed)
        future.wait();

    m_socket.reset();
}

uint16_t UdpMux::port() const
{
    return m_port;
}

std::optional<SocketAddress> UdpMux::mappedAddress() const
{
    NX_MUTEX_LOCKER lock(&m_mutex);
    return m_mappedAddress;
}

bool UdpMux::send(const SocketAddress& peer, const nx::Buffer& data)
{
    NX_MUTEX_LOCKER lock(&m_sendMutex);
    return m_socket->sendTo(data.data(), data.size(), peer);
}

void UdpMux::removeStoppedIces()
{
    std::vector<std::unique_ptr<IceUdpMuxed>> stopped;
    {
        NX_MUTEX_LOCKER lock(&m_mutex);
        for (auto it = m_iceBySource.begin(); it != m_iceBySource.end();)
        {
            if (it->second->isStopped())
            {
                stopped.push_back(std::move(it->second));
                it = m_iceBySource.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    for (auto& ice: stopped)
        destroyOnOwnAioThread(std::move(ice));
}

void UdpMux::readNext()
{
    m_socket->readSomeAsync(
        &m_readBuffer,
        [this](SystemError::ErrorCode errorCode, std::size_t bytesTransferred)
        {
            onBytesRead(errorCode, bytesTransferred);
        });
}

void UdpMux::onBytesRead(SystemError::ErrorCode errorCode, std::size_t bytesTransferred)
{
    // Windows reports an ICMP "port unreachable" from one gone peer as a read error of the whole
    // socket; it says nothing about the port, which serves the other peers on.
    const bool isErrorOfOnePeer =
        errorCode == SystemError::connectionReset || errorCode == SystemError::connectionRefused;
    if (socketCannotRecoverFromError(errorCode) && !isErrorOfOnePeer)
    {
        NX_WARNING(
            this,
            "Reading the shared WebRTC UDP port %1 failed: %2; the port serves no new peers "
            "until the server restarts",
            m_port,
            SystemError::toString(errorCode));
        return;
    }

    if (errorCode == SystemError::noError && bytesTransferred > 0)
    {
        const SocketAddress source = m_socket->lastDatagramSourceAddress();
        const nx::Buffer packet = std::exchange(m_readBuffer, nx::Buffer());
        m_readBuffer.reserve(kReadBufferSize);
        routePacket(source, packet);
    }
    else
    {
        m_readBuffer.resize(0);
    }
    readNext();
}

void UdpMux::routePacket(const SocketAddress& source, const nx::Buffer& packet)
{
    {
        NX_MUTEX_LOCKER lock(&m_mutex);
        if (const auto it = m_iceBySource.find(source.toString()); it != m_iceBySource.end())
        {
            it->second->deliver(packet);
            return;
        }
    }
    routeFromNewSource(source, packet);
}

void UdpMux::routeFromNewSource(const SocketAddress& source, const nx::Buffer& packet)
{
    // Only a check that proves the session's ice-pwd may claim a source address, so that neither
    // garbage nor spoofed sources can fill the table or take a session's path over.
    std::shared_ptr<Session> session;
    const auto sessionId = authenticatedSessionId(
        packet,
        [this, &session](const std::string& id) -> std::optional<std::string>
        {
            session = m_sessionPool->getSession(id).lock();
            return session ? std::optional<std::string>(session->localPwd()) : std::nullopt;
        });
    if (!sessionId)
    {
        NX_VERBOSE(this, "Unauthenticated datagram from %1 dropped", source);
        return;
    }

    const std::string key = source.toString();
    bool hasLivePath = false;
    bool isFull = false;
    {
        NX_MUTEX_LOCKER lock(&m_mutex);
        const auto path = std::find_if(
            m_iceBySource.begin(),
            m_iceBySource.end(),
            [&sessionId](const auto& entry)
            {
                const auto& ice = entry.second;
                return ice->sessionId() == *sessionId && ice->holdsSession() && !ice->isStopped();
            });
        if (path != m_iceBySource.end() && path->second->isSilentFor(kPeerMoveSilence))
        {
            auto ice = std::move(path->second);
            m_iceBySource.erase(path);
            ice->movePeer(source);
            ice->deliver(packet);
            m_iceBySource.emplace(key, std::move(ice));
            return;
        }
        hasLivePath = path != m_iceBySource.end();
        isFull = m_iceBySource.size() >= kMaxIceCount;
    }

    if (hasLivePath)
    {
        // Another candidate pair of the same peer, for example through its relay, is still
        // checking: the session keeps its live path.
        NX_VERBOSE(this, "Session %1 has a live path, check from %2 dropped", *sessionId, source);
        return;
    }

    // Checked before the ice exists: one destroyed here would block this thread on its own.
    if (isFull)
    {
        NX_DEBUG(
            this,
            "%1 peers on the shared UDP port already, %2 dropped",
            kMaxIceCount,
            source);
        return;
    }

    auto ice = std::make_unique<IceUdpMuxed>(
        m_sessionPool,
        session->config(),
        session->aioThread(),
        this,
        *sessionId,
        source,
        SocketAddress(HostAddress::anyHost, m_port));

    NX_DEBUG(this, "New peer %1 on the shared UDP port for session %2", source, *sessionId);
    NX_MUTEX_LOCKER lock(&m_mutex);
    ice->deliver(packet);
    m_iceBySource.emplace(key, std::move(ice));
}

void UdpMux::onMappingChanged(const SocketAddress& externalAddress)
{
    std::optional<SocketAddress> mapped;
    if (externalAddress.port != 0 && externalAddress.address != HostAddress())
        mapped = externalAddress;

    {
        NX_MUTEX_LOCKER lock(&m_mutex);
        if (m_mappedAddress == mapped)
            return;
        m_mappedAddress = mapped;
    }

    if (mapped)
    {
        NX_INFO(
            this,
            "The router maps %1 to the shared WebRTC UDP port %2 (UPnP)",
            *mapped,
            m_port);
    }
    else
    {
        NX_INFO(
            this,
            "The router dropped the UPnP mapping of the shared WebRTC UDP port %1; a peer that "
            "srflx cannot reach uses the TURN relay",
            m_port);
    }
}

} // namespace nx::webrtc
