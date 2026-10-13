// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "webrtc_session_pool.h"

#include <nx/vms/common/system_context.h>
#include <nx/vms/common/system_settings.h>

#include "webrtc_ice_tcp.h"
#include "webrtc_session.h"

namespace nx::webrtc {

namespace {

// Skip addresses that can never form a useful ICE host candidate for a remote peer:
// loopback (127/8, ::1) and link-local (169.254/16, fe80::/10).
bool isUsableIceHostAddress(const nx::network::SocketAddress& a)
{
    return !a.address.isLoopback() && !a.address.isIpv4LinkLocalNetwork()
        && !a.address.isIpv6LinkLocalNetwork();
}

} // namespace

SessionPool::SessionPool(
    const std::string realm,
    const std::string cloudHost,
    nx::vms::common::SystemContext* context,
    nx::utils::TimerManager* timerManager,
    QnFfmpegVideoTranscoder::TranscodePolicy transcodePolicy,
    const std::vector<nx::network::SocketAddress>& defaultStunServers):
    nx::vms::common::SystemContextAware(context),
    m_timerManager(timerManager),
    m_turnInfoFetcher(std::make_unique<TurnInfoFetcher>(realm, cloudHost)),
    m_transcodePolicy(transcodePolicy),
    m_defaultStunServers(defaultStunServers)
{
    static const std::chrono::milliseconds kMinTimeout(100);
    using namespace std::chrono;
    m_keepAliveTimeout = systemSettings()->webrtcSessionTimeout();
    const milliseconds timeout =
        std::max(kMinTimeout, milliseconds(m_keepAliveTimeout.count() / 4));
    using namespace std::placeholders;
    m_timerId = m_timerManager->addNonStopTimer(
        std::bind(&SessionPool::onTimer, this, _1),
        timeout,
        timeout);
}

SessionPool::~SessionPool()
{
    stop();
}

void SessionPool::stop()
{
    m_timerManager->joinAndDeleteTimer(m_timerId);

    // TODO move working ice to session and use simple crear() without swap and deletion order.
    // Now it should be deleted before sessions, since ices have pointer to session.
    decltype(m_sessionById) sessionById;
    decltype(m_tcpIces) icesTcp;
    {
        NX_MUTEX_LOCKER lk(&m_mutex);
        std::swap(icesTcp, m_tcpIces);
        std::swap(sessionById, m_sessionById);
    }
}

std::vector<nx::network::SocketAddress> SessionPool::getStunServers()
{
    std::vector<nx::network::SocketAddress> result;
    result.insert(result.end(), m_defaultStunServers.begin(), m_defaultStunServers.end());
    return result;
}

TurnInfoFetcher* SessionPool::turnInfoFetcher() const
{
    return m_turnInfoFetcher.get();
}

void SessionPool::setTurnCredentialProvider(TurnInfoFetcher::CredentialProvider provider)
{
    m_turnInfoFetcher->setCredentialProvider(std::move(provider));
}

std::string SessionPool::emitUfrag()
{
    NX_MUTEX_LOCKER lk(&m_mutex);

    std::string localUfrag;
    do
    {
        localUfrag = Session::generateLocalUfrag();
    } while (m_sessionById.count(localUfrag) > 0);

    m_sessionById[localUfrag] = nullptr;
    return localUfrag;
}

std::shared_ptr<Session> SessionPool::createInternal(
    createDataProviderFactory providerFactory,
    Purpose purpose,
    const nx::network::SocketAddress& address,
    const std::string& localUfrag,
    const SessionConfig& config,
    const QList<nx::network::SocketAddress>& hostAddresses,
    nx::network::aio::AbstractAioThread* aioThread)
{
    std::shared_ptr<Session> session;
    {
        NX_MUTEX_LOCKER lk(&m_mutex);
        const auto sessionIter = m_sessionById.find(localUfrag);
        if (!NX_ASSERT(
                sessionIter != m_sessionById.end() && !sessionIter->second,
                "Session ufrag should be taken before by takeUfrag() of emitUfrag()"))
        {
            return nullptr;
        }

        session = std::make_shared<Session>(
            systemContext(),
            this,
            std::move(providerFactory),
            localUfrag,
            address,
            config,
            getStunServers(),
            purpose,
            aioThread);

        m_sessionById[session->id()] = session;
    }

    // Compose the built-in filter (skip loopback + link-local) with the
    // optional caller-supplied filter (typically a per-server admin allow/deny
    // list). The factory is snapshotted under the mutex, then invoked once
    // *outside* the mutex to build a session-scoped filter.
    HostAddressFilterFactory factory;
    {
        NX_MUTEX_LOCKER lk(&m_mutex);
        factory = m_hostAddressExtraFilterFactory;
    }
    HostAddressFilter extraFilter;
    if (factory)
        extraFilter = factory();
    auto composed = [extraFilter =
                         std::move(extraFilter)](const nx::network::SocketAddress& a) -> bool
    {
        if (!isUsableIceHostAddress(a))
            return false;
        if (extraFilter && !extraFilter(a))
            return false;
        return true;
    };

    NX_DEBUG(this, "Started session %1", localUfrag);
    QList<nx::network::SocketAddress> addresses;
    for (const auto& hostAddress: hostAddresses)
    {
        if (composed(hostAddress))
            addresses.push_back(hostAddress);
    }
    session->start(addresses);
    return session;
}

void SessionPool::setHostAddressExtraFilterFactory(HostAddressFilterFactory factory)
{
    NX_MUTEX_LOCKER lk(&m_mutex);
    m_hostAddressExtraFilterFactory = std::move(factory);
}

std::shared_ptr<Session> SessionPool::create(
    createDataProviderFactory providerFactory,
    Purpose purpose,
    const nx::network::SocketAddress& address,
    const SessionConfig& config,
    const QList<nx::network::SocketAddress>& hostAddresses,
    nx::network::aio::AbstractAioThread* aioThread)
{
    const auto localUfrag = emitUfrag();
    return createInternal(
        std::move(providerFactory),
        purpose,
        address,
        localUfrag,
        config,
        hostAddresses,
        aioThread);
}

std::weak_ptr<Session> SessionPool::getSession(const std::string& id)
{
    // TODO: Maybe it is better to cleanup now unused Ices here...
    NX_MUTEX_LOCKER lk(&m_mutex);
    auto session = m_sessionById.find(id);
    if (session == m_sessionById.end() || !session->second)
        return std::weak_ptr<Session>();

    return session->second;
}

void SessionPool::onTimer(const quint64& /*timerId*/)
{
    std::vector<SessionPtr> sessionsToRemove;
    decltype(m_tcpIces) tcpIcesToRemove;
    {
        NX_MUTEX_LOCKER lk(&m_mutex);
        for (auto it = m_sessionById.begin(); it != m_sessionById.end();)
        {
            auto& ctx = it->second;
            if (ctx && ctx->hasExpired(m_keepAliveTimeout))
            {
                if (ctx->isStreaming())
                {
                    // A streaming session whose peer went silent (a hung browser
                    // tab, or a relay path dropped without consent checks) is
                    // being collected - distinct from an ordinary idle removal
                    // so a wedge is identifiable when debug logging is on.
                    NX_DEBUG(
                        this,
                        "Reaping streaming session %1: no inbound from peer for "
                        "%2 - peer likely gone (hung tab or dropped relay)",
                        ctx,
                        m_keepAliveTimeout);
                }
                else
                {
                    NX_DEBUG(this, "Remove idle session on timer: %1", ctx);
                }
                sessionsToRemove.push_back(std::move(it->second));
                it = m_sessionById.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (auto it = m_tcpIces.begin(); it != m_tcpIces.end();)
        {
            if ((*it)->isStopped())
            {
                tcpIcesToRemove.push_back(std::move(*it));
                it = m_tcpIces.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    for (auto& session: sessionsToRemove)
        Session::stop(std::move(session)); //< Sequences the safe destruction; see Session::stop().
}

void SessionPool::drop(const std::string& id)
{
    SessionPtr toRemove;
    {
        NX_MUTEX_LOCKER lk(&m_mutex);
        auto sessionIter = m_sessionById.find(id);
        if (sessionIter == m_sessionById.end())
            return; //< Already dropped by some way.
        NX_CRITICAL(sessionIter->second);
        toRemove = std::move(sessionIter->second);
        m_sessionById.erase(sessionIter);
        NX_DEBUG(this, "Dropped session %1", id);
    }
    Session::stop(std::move(toRemove)); //< Sequences the safe destruction; see Session::stop().
}

void SessionPool::gotIceTcp(std::unique_ptr<IceTcp>&& iceTcp)
{
    iceTcp->start();

    NX_MUTEX_LOCKER lk(&m_mutex);
    m_tcpIces.push_back(std::move(iceTcp));
}

void SessionPool::onTcpConnection(
    std::unique_ptr<nx::network::AbstractStreamSocket> socket,
    const QByteArray& readBuffer)
{
    gotIceTcp(std::make_unique<IceTcp>(std::move(socket), this, SessionConfig(), readBuffer));
}

QnFfmpegVideoTranscoder::TranscodePolicy SessionPool::transcodePolicy() const
{
    return m_transcodePolicy;
}

SessionPool::Stats SessionPool::stats() const
{
    std::vector<SessionPtr> sessions;
    {
        NX_MUTEX_LOCKER lk(&m_mutex);
        for (const auto& [id, session]: m_sessionById)
        {
            if (session && session->purpose() != Purpose::data)
                sessions.push_back(session);
        }
    }

    // Each session locks itself, so the cameras are counted with the pool unlocked.
    Stats result;
    result.sessions = (int) sessions.size();
    for (const auto& session: sessions)
        result.cameras += (int) session->allProviders().size();
    return result;
}

} // namespace nx::webrtc
