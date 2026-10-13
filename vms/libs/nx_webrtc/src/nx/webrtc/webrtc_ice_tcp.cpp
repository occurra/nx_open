// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "webrtc_ice_tcp.h"

#include <network/tcp_connection_priv.h>

#include "webrtc_session_pool.h"

namespace nx::webrtc {

constexpr size_t kRfc4571HeaderSize = 2;
constexpr size_t kRawReadBufferSize = 65536;

using namespace nx::network;

ListenerTcp::ListenerTcp(
    std::unique_ptr<nx::network::AbstractStreamSocket> socket,
    QnTcpListener* owner,
    SessionPool* sessionPool,
    int64_t maxTcpRequestSize):
    QnTCPConnectionProcessor(std::move(socket), owner, maxTcpRequestSize),
    m_sessionPool(sessionPool)
{
    setObjectName(nx::toString(this));
}

void ListenerTcp::run()
{
    Q_D(QnTCPConnectionProcessor);
    initSystemThreadId();
    NX_VERBOSE(this, "Running ice: request size = %1", d->clientRequest.size());

    auto ice =
        std::make_unique<IceTcp>(takeSocket(), m_sessionPool, SessionConfig(), d->clientRequest);

    m_sessionPool->gotIceTcp(std::move(ice));
}

IceTcp::IceTcp(
    std::unique_ptr<nx::network::AbstractStreamSocket>&& socket,
    SessionPool* sessionPool,
    const SessionConfig& config,
    const QByteArray& readBuffer):
    Ice(sessionPool, config),
    m_socket(std::move(socket), readBuffer)
{
    m_readBuffer.reserve(kRawReadBufferSize);

    m_socket.bindToAioThread(m_pollable.getAioThread());
    m_socket.setSendBufferSize(kIceTcpSendBufferSize);
    m_socket.setNonBlockingMode(true);
}

void IceTcp::start()
{
    startReading();
}

void IceTcp::startReading()
{
    m_socket.readSomeAsync(
        &m_readBuffer,
        [this](auto... args)
        {
            onBytesRead(std::move(args)...);
        });
}

IceCandidate::Filter IceTcp::type() const
{
    return IceCandidate::Filter::TcpHost;
}

IceTcp::~IceTcp()
{
    std::promise<void> stoppedPromise;
    m_pollable.dispatch(
        [this, &stoppedPromise]()
        {
            stopUnsafe();
            stoppedPromise.set_value();
        });
    stoppedPromise.get_future().wait();
    NX_VERBOSE(this, "Destroyed.");
}

void IceTcp::stopUnsafe()
{
    if (!m_needStop)
    {
        Ice::stopUnsafe();

        m_socket.cancelIOSync(aio::EventType::etAll);
    }
    m_stopped = true;
}

bool IceTcp::isStopped()
{
    return m_stopped;
}

nx::Buffer IceTcp::toSendBuffer(const char* data, int dataSize) const
{
    if (dataSize > std::numeric_limits<uint16_t>::max())
    {
        NX_DEBUG(
            this,
            "Can't send message: size is too large %1 for %2",
            dataSize,
            (m_sessionDescription ? m_sessionDescription->id : "(null)"));
        return nx::Buffer();
    }

    nx::Buffer packet;
    uint16_t size = htons((uint16_t) dataSize);
    packet.append((const char*) &size, kRfc4571HeaderSize);
    packet.append(data, dataSize);
    return packet;
}

void IceTcp::onBytesRead(SystemError::ErrorCode errorCode, std::size_t bytesTransferred)
{
    if (m_needStop)
        return;

    if (nx::network::socketCannotRecoverFromError(errorCode) || bytesTransferred == 0)
    {
        stopUnsafe();
        return;
    }

    processBufferedPackets();
};

void IceTcp::processBufferedPackets()
{
    while (m_readBuffer.size() >= kRfc4571HeaderSize)
    {
        char* data = m_readBuffer.data();
        uint16_t packetSize = qFromBigEndian(*(uint16_t*) data);
        if (m_readBuffer.size() < packetSize + kRfc4571HeaderSize)
            break; //< Frame is not complete yet.

        const PacketResult result =
            processIncomingPacket((uint8_t*) data + kRfc4571HeaderSize, packetSize);
        if (result == PacketResult::fatal)
        {
            stopUnsafe();
            return;
        }
        //< This packet named our session, so handleStun set m_pendingAioThread and
        //< returned WITHOUT answering it: leave it in the buffer and migrate. The replay
        //< on the session's thread processes it there, so its STUN response - and the
        //< dtls handshake, the session lock, the consumer after it - is written on that
        //< thread, and nothing is written on this one before the move. Frames the same
        //< read brought stay buffered and are drained on the far side, in order.
        if (m_pendingAioThread)
            break;
        m_readBuffer.erase(0, packetSize + kRfc4571HeaderSize);
    }

    if (m_pendingAioThread)
    {
        //< Move to the session's thread, but only once nothing is being sent: rebinding a
        //< monitored socket is illegal, and hasBufferToSend() covers the whole send window
        //< (see Ice). A STUN error answered for a packet before the naming one may still be
        //< in flight - then onBytesWritten posts the move when its queue drains, so exactly
        //< one move is ever queued. It is a posted call, not inline, because the socket also
        //< stays registered for etRead until this read handler returns.
        if (!hasBufferToSend())
            m_pollable.post(
                [this]()
                {
                    migrateToSessionAioThread();
                });
        return;
    }

    startReading();
}

void IceTcp::migrateToSessionAioThread()
{
    if (!m_pendingAioThread || m_needStop)
        return; //< Already moved, or the ice died while this call was queued.

    auto* const aioThread = m_pendingAioThread;
    m_pendingAioThread = nullptr;

    NX_DEBUG(
        this,
        "%1: moving to the session's aio thread",
        m_sessionDescription ? m_sessionDescription->id : "(null)");

    //< Legal in this state and no other: the caller posts this only with no send in flight
    //< (see processBufferedPackets / onBytesWritten), nothing is armed on the socket, and
    //< nothing posts to m_pollable before the session is locked - which happens later, on
    //< the far side of this move.
    m_socket.bindToAioThread(aioThread);
    m_pollable.bindToAioThread(aioThread);

    //< Lands on the new thread and picks up where the parse loop stopped.
    m_pollable.post(
        [this]()
        {
            processBufferedPackets();
        });
}

void IceTcp::onBytesWritten(SystemError::ErrorCode errorCode, std::size_t bytesTransferred)
{
    Ice::onBytesWritten(errorCode, bytesTransferred);

    //< A STUN error answered for a packet before the naming one held the move up (see
    //< processBufferedPackets). Now that its send has drained, post the move - gated on
    //< hasBufferToSend() so only the last completion posts and processBufferedPackets did
    //< not also post: exactly one move is ever queued.
    if (m_pendingAioThread && !m_needStop && !hasBufferToSend())
        m_pollable.post(
            [this]()
            {
                migrateToSessionAioThread();
            });
}

void IceTcp::asyncSendPacketUnsafe()
{
    // See IceUdp::asyncSendPacketUnsafe: same thread, so no trip through the reactor.
    m_socket.sendAsync(
        &m_sendingBuffer,
        [this](auto... args)
        {
            onBytesWritten(std::move(args)...);
        });
}

nx::network::SocketAddress IceTcp::iceRemoteAddress() const
{
    return m_socket.getForeignAddress();
}

nx::network::SocketAddress IceTcp::iceLocalAddress() const
{
    return m_socket.getLocalAddress();
}

} // namespace nx::webrtc
