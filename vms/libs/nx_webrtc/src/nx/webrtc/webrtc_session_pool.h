// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <unordered_map>

#include <QtCore/QList>

#include <nx/network/socket.h>
#include <nx/utils/move_only_func.h>
#include <nx/utils/thread/mutex.h>
#include <nx/utils/timer_manager.h>
#include <nx/vms/common/system_context_aware.h>

#include "abstract_camera_data_provider.h"
#include "webrtc_turn_info_fetcher.h"

class QByteArray;

namespace nx::webrtc {

class IceTcp;

/**
 * - Owns Session objects.
 * - Removes session if no one uses it during specified timeout. Session cannot be removed while
 * its id is locked. NOTE: It is recommended to lock session id before performing any operations
 * with session. NOTE: Specifying session timeout and working with no id lock can result in
 * undefined behavior. NOTE: Class methods are thread-safe.
 */
class NX_WEBRTC_API SessionPool: public QObject, public nx::vms::common::SystemContextAware
{
    Q_OBJECT

public:
    SessionPool(
        const std::string realm,
        const std::string cloudHost,
        nx::vms::common::SystemContext* context,
        nx::utils::TimerManager* timerManager,
        QnFfmpegVideoTranscoder::TranscodePolicy transcodePolicy,
        const std::vector<nx::network::SocketAddress>& defaultStunServers);
    virtual ~SessionPool();

    /**
     * Add new session.
     * @param providerFactory Camera data provider factory; null for
     *     datachannel-only (Purpose::data) sessions.
     * @param hostAddresses Raw local addresses to offer as ICE host
     *     candidates - the caller enumerates them (the media_server from its
     *     own server resource, tests from the local interfaces), so the pool
     *     has no hidden identity dependency. The built-in loopback/link-local
     *     filter and the extra host-address filter still apply on top.
     * @param aioThread Thread the session runs on - pass the AIO thread of
     *     the signalling socket that will be handed to Tracker::start(), so
     *     that socket never has to be rebound. Null lets the tracker pick.
     * @return Null if a session with the same id already exists.
     */
    std::shared_ptr<Session> create(
        createDataProviderFactory providerFactory,
        Purpose purpose,
        const nx::network::SocketAddress& address,
        const SessionConfig& config,
        const QList<nx::network::SocketAddress>& hostAddresses,
        nx::network::aio::AbstractAioThread* aioThread = nullptr);
    /**
     * @param session id
     * @return Nullptr, if session not found.
     */
    std::weak_ptr<Session> getSession(const std::string& id);

    /**
     * @param session id
     * Drop session at all, regardless of its status.
     */
    void drop(const std::string& id);

    /**
     * @return instance of TurnInfoFetcher
     */
    TurnInfoFetcher* turnInfoFetcher() const;

    /// Inject the TURN credential provider; forwarded to the TurnInfoFetcher.
    void setTurnCredentialProvider(TurnInfoFetcher::CredentialProvider provider);

    /**
     * Optional predicate applied on top of the built-in ICE host-address filter
     * (which already skips link-local). Returns true for addresses
     * that should be offered as ICE host candidates, false for addresses that
     * should be suppressed.
     */
    using HostAddressFilter = std::function<bool(const nx::network::SocketAddress&)>;

    /**
     * Factory that builds a HostAddressFilter for a single WebRTC session.
     * Called once per session at address-enumeration time; the returned
     * predicate is then invoked once per candidate address.
     *
     * Rationale: some filters (e.g. one that reads a JSON setting from
     * ServerSettings) benefit from a per-session snapshot — parsing once
     * per session instead of once per address, and guaranteeing every
     * address in the same session sees a consistent view of the config
     * even if it changes mid-enumeration.
     *
     * If unset (default), no extra filtering is applied and only the
     * built-in loopback/link-local filter runs.
     */
    using HostAddressFilterFactory = std::function<HostAddressFilter()>;
    void setHostAddressExtraFilterFactory(HostAddressFilterFactory factory);

    // API for IceTcp.
    void gotIceTcp(std::unique_ptr<IceTcp>&& iceTcp);

    /// Wrap a freshly-accepted RFC4571/STUN-over-TCP connection in an IceTcp and adopt it.
    /// `readBuffer` carries the bytes already read from the socket (the first STUN frame).
    /// Lets a host TCP listener hand off a TCP-ICE connection without constructing IceTcp
    /// itself (Ice subclasses are not exported from this library). The IceTcp runs on its own
    /// aio thread, so no QnLongRunnable/connection-processor machinery is involved.
    void onTcpConnection(
        std::unique_ptr<nx::network::AbstractStreamSocket> socket,
        const QByteArray& readBuffer);

    void stop();
    QnFfmpegVideoTranscoder::TranscodePolicy transcodePolicy() const;

    /** How many sessions carry media, and how many cameras they stream in all. */
    struct Stats
    {
        int sessions = 0;
        int cameras = 0;
    };

    Stats stats() const;

private:
    std::vector<nx::network::SocketAddress> getStunServers();
    std::string emitUfrag();
    std::shared_ptr<Session> createInternal(
        createDataProviderFactory providerFactory,
        Purpose purpose,
        const nx::network::SocketAddress& address,
        const std::string& localUfrag,
        const SessionConfig& config,
        const QList<nx::network::SocketAddress>& hostAddresses,
        nx::network::aio::AbstractAioThread* aioThread);

    /** Implementation of TimerEventHandler::onTimer. */
    void onTimer(const quint64& timerId);

private:
    mutable nx::Mutex m_mutex;
    std::unordered_map<std::string, SessionPtr> m_sessionById;
    nx::utils::TimerManager* m_timerManager = nullptr;
    std::unique_ptr<TurnInfoFetcher> m_turnInfoFetcher;
    quint64 m_timerId = 0;
    std::chrono::seconds m_keepAliveTimeout;
    std::list<std::unique_ptr<IceTcp>> m_tcpIces;
    QnFfmpegVideoTranscoder::TranscodePolicy m_transcodePolicy;
    std::vector<nx::network::SocketAddress> m_defaultStunServers;
    HostAddressFilterFactory m_hostAddressExtraFilterFactory; //< Nullable.
};

} // namespace nx::webrtc
