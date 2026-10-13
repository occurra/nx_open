// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "webrtc_session.h"

#include <nx/network/aio/aio_thread.h>
#include <nx/utils/crypt/linux_passwd_crypt.h>
#include <nx/utils/random.h>
#include <nx/vms/common/system_context.h>

#include "nx_webrtc_ini.h"
#include "webrtc_data_peer.h"
#include "webrtc_ice_relay.h"
#include "webrtc_ice_tcp.h"
#include "webrtc_ice_udp.h"
#include "webrtc_receiver.h"
#include "webrtc_session_pool.h"
#include "webrtc_srflx.h"
#include "webrtc_streamer.h"
#include "webrtc_tracker.h"

/* RFC list:
 * https://webrtcforthecurious.com/docs/03-connecting/ - Overview of connecting process
 * http://rtcweb-wg.github.io/jsep/ - JavaScript Session Establishment Protocol
 * https://datatracker.ietf.org/doc/html/rfc3264 - An Offer/Answer Model with the Session
 * Description Protocol (SDP) https://datatracker.ietf.org/doc/html/rfc5245 - Interactive
 * Connectivity Establishment (ICE) - old. https://datatracker.ietf.org/doc/html/rfc8445 -
 * Interactive Connectivity Establishment (ICE) https://datatracker.ietf.org/doc/html/rfc6544 - TCP
 * Candidates with Interactive Connectivity Establishment (ICE)
 * https://datatracker.ietf.org/doc/html/rfc4145 - TCP-Based Media Transport in the Session
 * Description Protocol (SDP) https://datatracker.ietf.org/doc/html/rfc4571 - Framing RTP and RTCP
 * over Connection-Oriented Transport https://datatracker.ietf.org/doc/html/rfc8489 - Session
 * Traversal Utilities for NAT (STUN) https://datatracker.ietf.org/doc/html/rfc8656 - Traversal
 * Using Relays around NAT (TURN) https://datatracker.ietf.org/doc/html/rfc5764 - DTLS Extension to
 * Establish Keys for the SRTP https://datatracker.ietf.org/doc/html/rfc3711 - The Secure Real-time
 * Transport Protocol (SRTP) https://datatracker.ietf.org/doc/html/rfc4585 - Extended RTP Profile
 * for RTCP-Based Feedback (RTP/AVPF)
 * */

namespace {

std::string generatePwd(int size)
{
    auto result = nx::crypt::generateSalt(size).toStdString();
    std::for_each(
        result.begin(),
        result.end(),
        [](char& c)
        {
            if (c == '.')
                c = '+';
        });
    return result;
}

static const AVCodecID kFallbackSrtpVideoCodec = AV_CODEC_ID_VP8;
static const AVCodecID kFallbackSrtpAudioCodec = AV_CODEC_ID_PCM_MULAW;

} // namespace

namespace nx::webrtc {

using namespace nx::network;

std::string Session::generateLocalUfrag()
{
    return generatePwd(4);
}

Session::Session(
    nx::vms::common::SystemContext* context,
    SessionPool* sessionPool,
    createDataProviderFactory providerFactory,
    const std::string& localUfrag,
    const nx::network::SocketAddress& address,
    const SessionConfig& config,
    const std::vector<nx::network::SocketAddress>& stunServers,
    Purpose purpose,
    nx::network::aio::AbstractAioThread* aioThread):
    SystemContextAware(context),
    m_purpose(purpose),
    m_localAddress(address),
    m_pem(ssl::Context::instance()->getDefaultCertificate()),
    m_tracker(std::make_shared<Tracker>(this, aioThread)),
    m_stunServers(stunServers),
    m_sessionPool(sessionPool),
    m_config(config)
{
    m_localUfrag = localUfrag;
    m_localPwd = generatePwd(24);
    m_tieBreaker = nx::utils::random::number<std::uint64_t>();
    m_dtls = std::make_shared<Dtls>(m_localUfrag, m_pem);

    m_aioThread = m_tracker->getAioThread(); //< Before anything that reads it.

    //< Join the session's thread, like everything else it owns. Not just for
    //< symmetry: ~Session normally runs inside the teardown task stop() posts
    //< to this very thread, and pleaseStopSync() on a timer belonging to some
    //< other thread would block-wait from inside an aio thread - which
    //< NX_CRITICALs in QnStoppableAsync::pleaseStopSync(). Safe here: the
    //< timer is untouched until startRelayFallbackTimerUnsafe().
    m_relayFallbackTimer.bindToAioThread(m_aioThread);

    m_tracks = std::make_unique<TracksForSend>(this);

    if (m_purpose == Purpose::data)
    {
        //< Datachannel-only endpoint: no consumer, no providers, no tracks.
        m_transceiver = std::make_unique<DataPeer>(this, m_aioThread);
        return;
    }
    m_transceiver = std::make_unique<Streamer>(this);

    if (m_purpose == Purpose::send)
    {
        m_consumer = std::make_unique<Consumer>(this);
        m_providerFactory = std::move(providerFactory);
    }
    else if (m_purpose == Purpose::recv)
    {
        auto video = std::make_unique<Track>(Track{
            .mid = 0,
            .payloadType = 96,
            .trackType = TrackType::video,
            .offerState = TrackState::active,
        });
        m_tracks->addTrack(std::move(video));
        auto audio = std::make_unique<Track>(Track{
            .mid = 1,
            .payloadType = 0,
            .trackType = TrackType::audio,
            .offerState = TrackState::active,
        });
        m_tracks->addTrack(std::move(audio));

        m_demuxer = std::make_unique<Demuxer>(m_tracks.get());
    }
}

Session::~Session()
{
    if (!m_quiesced)
        m_relayFallbackTimer.pleaseStopSync();
    m_reader.reset();
    releaseTracker(); //< Should be guarded.
}

void Session::start(const QList<nx::network::SocketAddress>& addresses)
{
    // Split horizon: private-scope addresses are useless to a peer outside
    // this network, and a dead candidate costs the peer real time - Chrome
    // keeps re-checking a higher-priority dead pair for ~1.3s before it
    // nominates the working one. So advertise private addresses only after
    // the peer has proven to be behind the same NAT: its reflexive (srflx)
    // public IP equals ours. Until then they wait in m_deferredHostAddresses;
    // gotIceFromTracker() releases or drops them. If our own srflx fails
    // (isolated network) nobody can be classified - advertise everything
    // immediately, as before the split.
    QList<nx::network::SocketAddress> advertised = addresses;
    if (ini().webrtcSplitHorizonHostCandidates)
    {
        advertised.clear();
        for (const auto& address: addresses)
        {
            if (isPrivateScope(address.address))
                m_deferredHostAddresses.push_back(address);
            else
                advertised.push_back(address);
        }
    }

    // Candidate types this server may use at all. webrtcIceCandidates forces a
    // set, so a type left out of it is never gathered - filtering only what
    // sendIce() advertises forces nothing: the sockets still exist and still
    // punch out to the peer's candidates, which the peer then validates as
    // peer-reflexive, so an excluded type still wins the handshake. Empty (the
    // default) means all of them, so nothing changes unless the ini says so.
    auto candidateFilter = IceCandidate::Filter::All;
    if (const std::string types = ini().webrtcIceCandidates; !types.empty())
    {
        candidateFilter = IceCandidate::parseFilter(types);
        NX_DEBUG(this, "%1: ice candidates limited to %2", id(), types);
    }

    int priority = IceCandidate::kIceMaxPriority;
    int index = 0;
    // TCP host candidates.
    if (candidateFilter & IceCandidate::Filter::TcpHost)
    {
        for (const auto& address: advertised)
        {
            m_iceCandidates.push_back(
                IceCandidate::constructTcpHost(index++, priority--, address));
        }
    }

    // UDP host candidates.
    if (candidateFilter & IceCandidate::Filter::UdpHost)
    {
        for (const auto& address: advertised)
        {
            auto udpIce = std::make_unique<IceUdp>(m_sessionPool, m_config, aioThread());
            auto iceLocalAddress = udpIce->bind(address.address);
            if (iceLocalAddress)
            {
                m_iceCandidates.push_back(
                    IceCandidate::constructUdpHost(index++, priority--, *iceLocalAddress));
                m_udpIces.push_back(std::move(udpIce));
            }
        }
    }

    // Srflx candidate.
    // Use first 2 accessible STUN servers from list.
    // The probe ends in a punched udp ice, so it belongs to the srflx type
    // for filtering purposes.
    if (candidateFilter & IceCandidate::Filter::UdpSrflx)
    {
        m_srflx = std::make_unique<Srflx>(
            m_localUfrag,
            m_stunServers,
            [this](
                const nx::network::SocketAddress& mappedAddress,
                std::unique_ptr<nx::network::AbstractDatagramSocket>&& socket)
            {
                NX_DEBUG(
                    this,
                    "%1: Got Srflx remote addres: %2, local address: %3",
                    id(),
                    mappedAddress.toString(),
                    socket ? socket->getLocalAddress().toString() : "null");
                // 1. Create IceUdp with given socket.
                nx::network::SocketAddress iceLocalAddress;
                if (socket && !mappedAddress.isNull())
                {
                    iceLocalAddress = socket->getLocalAddress();

                    NX_MUTEX_LOCKER lk(&m_mutex);
                    m_punchedUdpIce =
                        std::make_unique<IceUdp>(m_sessionPool, m_config, aioThread());
                    m_punchedUdpIce->resetSocket(std::move(socket));
                    if (m_remoteSrflx)
                        m_punchedUdpIce->sendBinding(*m_remoteSrflx.get(), describeUnsafe());
                }

                // 2. Add candidate to list.
                if (!mappedAddress.isNull())
                {
                    IceCandidate srflxCandidate = IceCandidate::constructUdpSrflx(
                        IceCandidate::kIceMinIndex,
                        IceCandidate::kIceMaxPriority,
                        mappedAddress,
                        iceLocalAddress);
                    addIceCandidate(std::move(srflxCandidate));
                }

                // 3. Feed the split-horizon classifier: it needs our reflexive
                // IP to recognize a same-NAT peer. A null mappedAddress means we
                // cannot classify anyone - release the deferred candidates.
                {
                    NX_MUTEX_LOCKER lk(&m_mutex);
                    m_reflexiveIp = mappedAddress.address;
                    if (mappedAddress.isNull())
                        advertiseDeferredHostCandidatesUnsafe("srflx failed");
                    else
                        classifyPeerUnsafe();
                }
            },
            aioThread());
    }

    if (candidateFilter & IceCandidate::Filter::UdpRelay)
    {
        m_relayIce = std::make_unique<IceRelay>(
            m_sessionPool,
            m_config,
            id(),
            [this](const IceCandidate& candidate)
            {
                addIceCandidate(candidate);
            },
            aioThread());
    }

    // Relay is allocated lazily — see armRelayFallbackUnsafe() / startRelayNow().
}

void Session::gotIceFromTracker(const IceCandidate& iceCandidate)
{
    NX_DEBUG(this, "Got ice candidate from peer: %1 -> %2", id(), iceCandidate.serialize());

    // A null address is the end-of-candidates signal; TCP candidates carry
    // ports we must not punch UDP toward - our TCP host ices are passive.
    if (iceCandidate.address.isNull() || iceCandidate.protocol != IceCandidate::Protocol::UDP)
        return;

    NX_MUTEX_LOCKER lk(&m_mutex);

    // IPv6 has no NAT and no srflx punch machinery - the outbound binding
    // from the advertised host address itself is what opens our stateful
    // firewall's pinhole for the peer's connectivity checks. Both srflx- and
    // host-type candidates drive this punch: a browser with no NAT66 prunes
    // the redundant v6 srflx (RFC 8445 #5.1.3) and trickles the address as a
    // host candidate instead. Relay candidates must not - latching onto the
    // peer's TURN allocation would burn the direct path. sendBinding ignores
    // family and scope mismatches (see IceUdp::canPunchTo), an ice punches at
    // most one target (isPunching), and v4 host ices intentionally keep the
    // old passive behavior.
    if (iceCandidate.type == IceCandidate::Type::Host
        || iceCandidate.type == IceCandidate::Type::Srflx)
    {
        for (auto& udpIce: m_udpIces)
        {
            if (udpIce->isBoundToV6() && !udpIce->isPunching())
                udpIce->sendBinding(iceCandidate, describeUnsafe());
        }
    }

    // The peer's relayed address is the one peer address guaranteed to match
    // what OUR TURN server observes: a CGNAT peer reaches distinct
    // destinations from distinct public IPs, so the srflx it signals (some
    // STUN server's view of it) may never appear at the TURN server, and its
    // true TURN-facing address is in no candidate at all - the peer itself
    // does not know it. Permitting the peer's relay makes the relay-to-relay
    // pair NAT-independent. Deliberately nothing else: punching at a TURN
    // allocation would burn the direct path, and the relay address is the
    // TURN server's own IP, so it must not feed the split-horizon classifier.
    if (iceCandidate.type == IceCandidate::Type::Relay)
    {
        if (m_relayIce) //< Absent when webrtcIceCandidates excludes relay.
            m_relayIce->addRemotePeer(iceCandidate);
        //< A CGNAT peer may signal no usable srflx at all - the relay
        //< candidate must be able to arm the fallback on its own.
        armRelayFallbackUnsafe();
        return;
    }

    // Everything below is srflx-only machinery: the v4 hole punch from the
    // srflx socket, the relay's notion of the peer's reflexive address, and
    // the relay fallback timer. A host candidate must not touch it - latching
    // the srflx punch onto the peer's private host address would break the
    // real punch.
    if (iceCandidate.type != IceCandidate::Type::Srflx)
        return;

    if (m_punchedUdpIce && !m_punchedUdpIce->isPunching())
        m_punchedUdpIce->sendBinding(iceCandidate, describeUnsafe());

    m_remoteSrflx = std::make_unique<IceCandidate>(iceCandidate);
    if (m_relayIce) //< Absent when webrtcIceCandidates excludes relay.
        m_relayIce->addRemotePeer(iceCandidate);
    //< Armed even with no relay to fall back to: its handler is also what
    //< releases the deferred split-horizon host candidates, which are the only
    //< way a same-NAT peer ever learns a private address of ours.
    armRelayFallbackUnsafe();

    // The peer's srflx candidate carries its public IP - the only address of
    // the peer a browser exposes (host candidates hide behind mDNS names).
    // Collect it and try to classify the peer for the split horizon.
    m_peerReflexiveIps.push_back(iceCandidate.address.address);
    classifyPeerUnsafe();
}

void Session::classifyPeerUnsafe()
{
    if (m_deferredHostAddresses.empty() || !m_reflexiveIp)
        return; //< Nothing deferred, or our own reflexive IP is not known yet.

    for (const auto& peerIp: m_peerReflexiveIps)
    {
        if (sameIp(peerIp, *m_reflexiveIp))
        {
            advertiseDeferredHostCandidatesUnsafe("peer is behind the same NAT");
            return;
        }
    }
    // No match (yet). Keep waiting: the peer may still trickle more srflx
    // candidates, e.g. its v4 one after a v6 one. A peer that never matches
    // simply never receives the private candidates - which is the point.
}

void Session::advertiseDeferredHostCandidatesUnsafe(const std::string& reason)
{
    if (m_deferredHostAddresses.empty())
        return;

    NX_DEBUG(
        this,
        "%1: Advertising %2 deferred private host candidate(s): %3",
        id(),
        m_deferredHostAddresses.size(),
        reason);

    // The foundation/priority constructor args below are placeholders:
    // addIceCandidateUnsafe renumbers every trickled candidate to continue the
    // session's descending priority sequence, so deferred private candidates
    // always sort below everything advertised before them.
    const auto deferred = std::exchange(m_deferredHostAddresses, {});
    for (const auto& address: deferred)
    {
        addIceCandidateUnsafe(
            IceCandidate::constructTcpHost(
                IceCandidate::kIceMinIndex,
                IceCandidate::kIceMaxPriority,
                address));

        auto udpIce = std::make_unique<IceUdp>(m_sessionPool, m_config, aioThread());
        auto iceLocalAddress = udpIce->bind(address.address);
        if (iceLocalAddress)
        {
            addIceCandidateUnsafe(
                IceCandidate::constructUdpHost(
                    IceCandidate::kIceMinIndex,
                    IceCandidate::kIceMaxPriority,
                    *iceLocalAddress));
            m_udpIces.push_back(std::move(udpIce));
        }
    }
}

/**
 * How many times a punch success response may extend the relay fallback timer
 * before it fires anyway. Bounds the extra wait (N * webrtcRelayFallbackDelayMs)
 * for the corner case where our checks reach the peer but the peer's own checks
 * never validate - the relay must still eventually start.
 */
static constexpr int kMaxRelayFallbackExtensions = 10;

void Session::armRelayFallbackUnsafe()
{
    if (m_relayArmed || m_relayStarted)
        return;
    m_relayArmed = true;
    startRelayFallbackTimerUnsafe();
}

void Session::startRelayFallbackTimerUnsafe()
{
    m_directProbeSucceeded = false;
    const auto delay = std::chrono::milliseconds(ini().webrtcRelayFallbackDelayMs);
    m_relayFallbackTimer.start(
        delay,
        [this]()
        {
            onRelayFallbackTimeout();
        });
}

void Session::onRelayFallbackTimeout()
{
    {
        NX_MUTEX_LOCKER lk(&m_mutex);

        // A success response to our own connectivity check arrived since the
        // timer was (re)armed: the direct path works end-to-end and the peer's
        // authenticated check is likely in flight (its arrival is what cancels
        // this timer). Releasing the deferred private candidates now would let
        // a VPN pair (e.g. Tailscale) validate and outrank that in-progress
        // direct handshake in the peer's pair ordering - so wait another
        // period instead. Bounded, because the response is unauthenticated.
        if (m_directProbeSucceeded
            && ++m_relayFallbackExtensionCount <= kMaxRelayFallbackExtensions)
        {
            NX_DEBUG(
                this,
                "%1: Direct path probe succeeded; extending relay fallback (%2/%3)",
                id(),
                m_relayFallbackExtensionCount,
                kMaxRelayFallbackExtensions);
            startRelayFallbackTimerUnsafe();
            return;
        }

        // No direct-path evidence within the last period (or extensions
        // exhausted): the deferred private candidates can no longer stall a
        // working direct pair - and for a peer whose only route is a VPN
        // (e.g. Tailscale to a firewalled Server) they are the last direct
        // hope. Trickle them and let them race the relay: the relay locks
        // only if they lose.
        advertiseDeferredHostCandidatesUnsafe("relay fallback, last direct hope");
    }
    startRelayNow();
}

void Session::onDirectPathProbeSucceeded()
{
    NX_MUTEX_LOCKER lk(&m_mutex);
    m_directProbeSucceeded = true;
}

void Session::startRelayNow()
{
    {
        NX_MUTEX_LOCKER lk(&m_mutex);
        if (m_relayStarted || m_stopped || !m_relayIce)
            return;
        m_relayStarted = true;
    }

    const auto info = m_sessionPool->turnInfoFetcher()->turnServerInfo();
    if (info.isNull())
    {
        NX_WARNING(this, "Relay fallback: no TURN server/creds; staying STUN-only for %1", id());
        return;
    }
    NX_DEBUG(this, "Relay fallback: allocating TURN relay for %1", id());
    m_relayIce->startRelay(info);
}

void Session::onDirectConnectivityEstablished()
{
    {
        NX_MUTEX_LOCKER lk(&m_mutex);
        if (m_relayStarted)
            return; // already relaying or past the decision point
        m_relayStarted = true; // suppress any pending/future fallback
    }
    m_relayFallbackTimer.cancelSync();
    NX_DEBUG(this, "Direct connectivity established; relay fallback cancelled for %1", id());
}

AbstractCameraDataProviderPtr Session::createProvider(
    nx::Uuid deviceId,
    std::optional<std::chrono::milliseconds> positionMs,
    nx::vms::api::StreamIndex stream,
    std::optional<float> speedOpt)
{
    // Idempotent: a device already streaming is a no-op, not a second provider. Lets a client
    // that lists its cameras in the open URL (so the server builds them all before the first
    // offer) still re-add them over the data channel for older servers, without duplicating.
    if (auto existing = provider(deviceId))
        return existing;

    // One offer/answer round at a time: an add arriving while an offer is on the wire is
    // deferred, not applied, so its track never appears in the current round's answer (which
    // would look like a peer rejection and force a spurious re-offer). It is applied and offered
    // together with any siblings once the round lands - see onOfferAnswered().
    if (m_offerInProgress)
    {
        NX_DEBUG(this, "%1: offer in flight, queuing add for device %2", id(), deviceId);
        m_pendingCommands.push_back(
            PendingCommand{PendingCommand::Op::add, deviceId, positionMs, stream, speedOpt});
        return AbstractCameraDataProviderPtr();
    }

    auto provider = createProviderNow(deviceId, positionMs, stream, speedOpt);
    if (provider && m_tracker && m_tracker->isStarted())
        m_tracker->sendOffer();
    return provider;
}

AbstractCameraDataProviderPtr Session::createProviderNow(
    nx::Uuid deviceId,
    std::optional<std::chrono::milliseconds> positionMs,
    nx::vms::api::StreamIndex stream,
    std::optional<float> speedOpt)
{
    auto provider = m_providerFactory(deviceId, positionMs, stream, speedOpt);
    // The consumer owns the session's metadata subscription state (seeded from
    // the signaling URL, toggled by the {"metadata": bool} data-channel
    // command). Apply it to every new provider so streams added mid-session
    // follow the current subscription, not the state at session open.
    provider->setEnableMetadata(m_consumer->isMetadataEnabled());
    addProvider(provider);

    if (!initializeMuxersInternal())
    {
        NX_WARNING(this, "Can't create data provider for device %1", deviceId);
        return AbstractCameraDataProviderPtr();
    }
    m_addedSinceOffer.push_back(deviceId);
    return provider;
}

bool Session::removeProvider(nx::Uuid deviceId)
{
    if (m_offerInProgress)
    {
        NX_DEBUG(this, "%1: offer in flight, queuing remove for device %2", id(), deviceId);
        m_pendingCommands.push_back(
            PendingCommand{
                PendingCommand::Op::remove,
                deviceId,
                std::nullopt,
                nx::vms::api::StreamIndex::primary,
                std::nullopt});
        return true;
    }

    const bool removed = removeProviderNow(deviceId);
    if (removed && m_tracker && m_tracker->isStarted())
        m_tracker->sendOffer();
    return removed;
}

bool Session::removeProviderNow(nx::Uuid deviceId)
{
    std::shared_ptr<AbstractCameraDataProvider> p;
    {
        NX_MUTEX_LOCKER lock(&m_mutex);
        auto it = m_providers.find(deviceId);
        if (it == m_providers.end())
            return false;
        p = it->second;
        m_providers.erase(it);
        m_unsupportedVideoCodecs.erase(deviceId);
        // A device gone from the session must not stay pending an offer answer: its answer may
        // be dropped as stale (a newer offer is out) and never clear it, and offerInProgress()
        // would then hold this device's media at the queue front forever - wedging the session.
        m_devicesAwaitingAnswer.erase(deviceId);
        std::erase(m_addedSinceOffer, deviceId);
    }
    p->stop();
    if (auto track = m_tracks->videoTrack(deviceId))
    {
        track->offerState = TrackState::inactive;
        track->useTranscoding = false;
    }
    if (auto track = m_tracks->audioTrack(deviceId))
    {
        track->offerState = TrackState::inactive;
        track->useTranscoding = false;
    }
    return true;
}

void Session::markOfferSent()
{
    m_offerInProgress = true;
    NX_MUTEX_LOCKER lock(&m_mutex);
    for (const auto& deviceId: m_addedSinceOffer)
        m_devicesAwaitingAnswer.insert(deviceId);
    m_addedSinceOffer.clear();
}

void Session::onOfferAnswered()
{
    m_offerInProgress = false;
    {
        NX_MUTEX_LOCKER lock(&m_mutex);
        m_devicesAwaitingAnswer.clear();
    }
    drainPendingCommands();
}

bool Session::offerInProgress(nx::Uuid deviceId) const
{
    NX_MUTEX_LOCKER lock(&m_mutex);
    return m_devicesAwaitingAnswer.contains(deviceId);
}

void Session::drainPendingCommands()
{
    if (m_pendingCommands.empty())
        return;

    // Apply the whole queued batch without offering per command, then send a single offer for
    // the resulting state - a rapid burst of adds collapses into one renegotiation.
    bool changed = false;
    while (!m_pendingCommands.empty())
    {
        const auto cmd = m_pendingCommands.front();
        m_pendingCommands.pop_front();
        if (cmd.op == PendingCommand::Op::add)
        {
            if (!provider(cmd.deviceId)
                && createProviderNow(cmd.deviceId, cmd.positionMs, cmd.stream, cmd.speedOpt))
            {
                changed = true;
            }
        }
        else if (removeProviderNow(cmd.deviceId))
        {
            changed = true;
        }
    }

    if (changed && m_tracker && m_tracker->isStarted())
        m_tracker->sendOffer();
}

std::vector<nx::Uuid> Session::setCameras(
    const std::vector<CameraRequest>& cameras,
    std::optional<std::chrono::milliseconds> positionMs,
    std::optional<Consumer::RtpRebase> rebase)
{
    const auto isDesired = [&cameras](const nx::Uuid& deviceId)
    {
        for (const auto& c: cameras)
            if (c.deviceId == deviceId)
                return true;
        return false;
    };

    // Providers present now but no longer listed, lowest mid first: the peer pairs its departing
    // tiles with the arriving cameras in that order, and the retarget below must land each
    // arriving camera on the m-line the peer bound its tile to.
    std::vector<nx::Uuid> toRemove;
    {
        NX_MUTEX_LOCKER lock(&m_mutex);
        for (const auto& entry: m_providers)
            if (!isDesired(entry.first))
                toRemove.push_back(entry.first);
    }
    toRemove = m_tracks->orderByMid(std::move(toRemove));

    // Listed cameras not yet streaming. provider() locks internally, so this runs without the
    // lock held above.
    std::vector<CameraRequest> toAdd;
    for (const auto& c: cameras)
        if (!provider(c.deviceId))
            toAdd.push_back(c);

    if (toAdd.empty() && toRemove.empty())
        return {}; //< Already reconciled - nothing to renegotiate.

    // Start each arriving camera's live stream now, so its first keyframe is ready by the time the
    // codec probe in createProviderNow/retargetProviderNow waits on it below - a batch warms up in
    // parallel instead of one blocking probe at a time. Live only: an archive add plays from the
    // file, not the live stream. No-op until the media server installs the hook.
    if (m_warmUpProvider && (!positionMs || positionMs->count() < 0))
    {
        for (const auto& c: toAdd)
            m_warmUpProvider(c.deviceId, c.stream);
    }

    // A round is in flight: queue the whole diff. drainPendingCommands() applies it as one
    // coalesced offer once the answer lands, so this never puts a second offer on the wire.
    if (m_offerInProgress)
    {
        for (const auto& c: toAdd)
        {
            m_pendingCommands.push_back(
                PendingCommand{
                    PendingCommand::Op::add,
                    c.deviceId,
                    positionMs,
                    c.stream,
                    std::nullopt});
        }
        for (const auto& deviceId: toRemove)
        {
            m_pendingCommands.push_back(
                PendingCommand{
                    PendingCommand::Op::remove,
                    deviceId,
                    std::nullopt,
                    nx::vms::api::StreamIndex::primary,
                    std::nullopt});
        }
        return {};
    }

    // Pair each departing camera with an arriving one and retarget its m-line in place - same
    // mid, ssrc and timeline, no offer, the way setCamera switches a single tile. Only the net
    // difference in camera count changes the SDP: extra arrivals get a fresh m-line, extra
    // departures are parked inactive, and that alone is offered, once.
    const size_t pairs = std::min(toRemove.size(), toAdd.size());
    std::vector<nx::Uuid> retargeted;
    for (size_t i = 0; i < pairs; ++i)
    {
        if (retargetProviderNow(
                toRemove[i],
                toAdd[i].deviceId,
                positionMs,
                toAdd[i].stream,
                /*keyframeOnly*/ true,
                rebase))
        {
            retargeted.push_back(toAdd[i].deviceId);
        }
    }

    bool topologyChanged = false;
    for (size_t i = pairs; i < toAdd.size(); ++i)
    {
        if (createProviderNow(toAdd[i].deviceId, positionMs, toAdd[i].stream, std::nullopt))
            topologyChanged = true;
    }
    for (size_t i = pairs; i < toRemove.size(); ++i)
    {
        if (removeProviderNow(toRemove[i]))
            topologyChanged = true;
    }

    if (topologyChanged && m_tracker && m_tracker->isStarted())
        m_tracker->sendOffer();

    return retargeted;
}

AbstractCameraDataProviderPtr Session::setCamera(
    nx::Uuid deviceId,
    std::optional<std::chrono::milliseconds> positionMs,
    nx::vms::api::StreamIndex stream,
    bool keyframeOnly,
    std::optional<Consumer::RtpRebase> rebase)
{
    nx::Uuid fromDeviceId;
    {
        NX_MUTEX_LOCKER lock(&m_mutex);
        if (m_providers.size() != 1)
        {
            NX_WARNING(
                this,
                "setCamera needs a single streaming camera, %1 are streaming",
                m_providers.size());
            return AbstractCameraDataProviderPtr();
        }
        const auto& previous = m_providers.begin()->second;
        if (previous->deviceId() == deviceId)
            return previous; //< Already on air; repositioning it is a plain seek.
        fromDeviceId = previous->deviceId();
    }
    return retargetProviderNow(fromDeviceId, deviceId, positionMs, stream, keyframeOnly, rebase);
}

AbstractCameraDataProviderPtr Session::retargetProviderNow(
    nx::Uuid fromDeviceId,
    nx::Uuid toDeviceId,
    std::optional<std::chrono::milliseconds> positionMs,
    nx::vms::api::StreamIndex stream,
    bool keyframeOnly,
    std::optional<Consumer::RtpRebase> rebase)
{
    std::shared_ptr<AbstractCameraDataProvider> previous;
    bool wasRefused = false;
    {
        NX_MUTEX_LOCKER lock(&m_mutex);
        auto it = m_providers.find(fromDeviceId);
        if (it == m_providers.end())
            return AbstractCameraDataProviderPtr();
        previous = it->second;
        m_providers.erase(it);
        wasRefused = m_unsupportedVideoCodecs.erase(fromDeviceId) > 0;
    }
    previous->stop();

    // Hand the negotiated tracks and their encoders to the new device. The peer
    // knows nothing of device ids - it sees the same m-lines, payload types and
    // ssrcs - so the switch needs no offer/answer and the stream never stops.
    // The tracks keep their offerState: unlike removeProvider() this is not a
    // teardown, and marking them inactive would re-advertise them on the next
    // offer this session happens to send.
    m_tracks->retargetDevice(fromDeviceId, toDeviceId);
    m_muxer->retargetDevice(fromDeviceId, toDeviceId);
    if (wasRefused)
    {
        // The peer holds a refused camera's m-lines inactive, and they carry its fallback mark.
        // The camera taking them over is judged on its own: active again, its codec as it is.
        for (auto& [ssrc, track]: m_tracks->tracks())
        {
            if (track->deviceId != toDeviceId)
                continue;
            track->offerState = TrackState::active;
            track->useTranscoding = false;
        }
    }

    // Realtime, like the session was opened at. The provider takes its speed at
    // construction, and without one it paces nothing - the new camera's archive
    // would stream as fast as it can be read. Hardcoded because no command
    // carries a speed today; when one does, take it from there.
    auto provider = m_providerFactory(toDeviceId, positionMs, stream, /*speed*/ 1.0);
    if (!provider)
    {
        NX_WARNING(this, "Can't create data provider for device %1", toDeviceId);
        return AbstractCameraDataProviderPtr();
    }
    provider->setEnableMetadata(m_consumer->isMetadataEnabled());
    addProvider(provider);
    // The moved encoders are this device's now, so the provider is already muxable: mark it
    // initialized so a later initializeMuxersInternal (e.g. from the answer to an audio-only
    // re-offer) skips it and never rebuilds - and thereby reseeds - the untouched video encoder.
    // A genuine video codec change still forces a rebuild by clearing the flag itself.
    provider->setInitialized(true);

    // Start it, through seek() so the switch can carry the client's rebase the
    // same way an ordinary seek does: the marker rides the jump (stamped onto
    // this camera's packets), while the muxer is told separately which marker
    // restarts the output timeline. On a provider with nothing open yet, seek()
    // falls through to play(), so this is the plain start when no rebase is
    // asked for. Providers are otherwise started by the session's opening seek
    // (Streamer::start) or by the answer to the offer createProvider sends - a
    // switch has neither, so without this the camera is registered and silent.
    // Called straight, no dispatch: this runs on the data-channel command's aio
    // thread, which is the session's only thread and the one the muxer runs on.
    std::optional<int> marker;
    if (rebase && rebase->seq != 0)
    {
        marker = rebase->seq;
        if (rebase->rtpBase90k >= 0)
        {
            // Archive: the reader stamps the marker on the first post-seek packet, so the pending
            // rebase fires there and the pre-seek flush frames stay on the old timeline. Live: no
            // flush (the old provider is stopped) and seek() falls through to play() without
            // stamping a marker, so the pending path never fires - rebase the reused encoder now.
            if (positionMs)
                m_muxer->setPendingRtpRebase(toDeviceId, rebase->seq, rebase->rtpBase90k);
            else
                m_muxer->rebaseRtpNow(toDeviceId, rebase->rtpBase90k);
        }
    }
    provider->seek(provider->getPosition(), keyframeOnly, marker);

    // Deliberately no initializeMuxersInternal() and no sendOffer(): the
    // encoders the previous camera negotiated are now this device's, and
    // building new ones would mint a new ssrc and a new m-line. A codec the
    // existing encoder cannot take is caught where every other codec change is
    // - the first packet fails to encode and the Consumer renegotiates.
    NX_DEBUG(
        this,
        "Camera switched in place: %1 -> %2 (position: %3)",
        fromDeviceId,
        toDeviceId,
        positionMs ? std::to_string(positionMs->count()) : std::string("live"));

    // Inactive m-lines carry nothing until the peer agrees to reopen them.
    if (wasRefused)
    {
        m_addedSinceOffer.push_back(toDeviceId);
        if (m_tracker && m_tracker->isStarted())
            m_tracker->sendOffer();
    }

    return provider;
}

bool Session::adaptToAudioCodecChange(nx::Uuid deviceId, const QnConstAbstractMediaDataPtr& media)
{
    //< Video reaches the peer untranscoded, so a new video codec always needs a
    //< new SDP. Only audio is convertible back into what the peer expects.
    if (media->dataType != QnAbstractMediaData::AUDIO)
        return false;

    //< A codec the peer receives natively should reach it as it is, which means
    //< a new payload type and a new offer.
    if (m_muxer->isAudioCodecSupported(media->compressionType))
        return false;

    auto* encoder = m_muxer->audioEncoder(deviceId);
    auto provider = this->provider(deviceId);
    if (!encoder || !provider)
        return false;

    //< Seed the transcoder input from the packet itself rather than re-probing
    //< the archive, exactly as the renegotiation path does. Probing here would
    //< read the file on the muxing thread, stalling RTP output.
    if (const auto& context = media->getContext())
        provider->setAudioCodecParameters(context);

    const AVCodecID negotiated = encoder->sourceCodec();
    if (!provider->addAudioTranscoder(negotiated))
        return false;

    NX_DEBUG(
        this,
        "%1: Absorbed audio codec change %2 -> %3 for device %4, "
        "transcoding into the negotiated codec",
        id(),
        media->compressionType,
        negotiated,
        deviceId);

    encoder->clearEof();
    return true;
}

bool Session::hasTrackForMedia(const QnConstAbstractMediaDataPtr& media)
{
    NX_MUTEX_LOCKER lock(&m_mutex);
    if (media->dataType == QnAbstractMediaData::VIDEO)
        return m_tracks->videoTrack(media->deviceId) != nullptr;
    if (media->dataType == QnAbstractMediaData::AUDIO)
        return m_tracks->audioTrack(media->deviceId) != nullptr;
    return true;
}

void Session::beginCodecRenegotiation(nx::Uuid deviceId)
{
    auto provider = this->provider(deviceId);
    if (!provider)
        return;

    //< A device that joined without a stream (an offline camera negotiates nothing)
    //< has no track for it; the codec parameters seeded from the triggering packet
    //< are what addProvider was missing, so mint the track now and the rebuild below
    //< has an m-line to negotiate.
    {
        NX_MUTEX_LOCKER lock(&m_mutex);
        if (provider->getVideoCodecParameters() && !m_tracks->videoTrack(deviceId))
        {
            NX_DEBUG(this, "%1: minting a late video track for device %2", id(), deviceId);
            m_tracks->addTrack(
                std::make_unique<Track>(Track{
                    .deviceId = deviceId,
                    .trackType = TrackType::video,
                }));
        }
        if (provider->getAudioCodecParameters() && !m_tracks->audioTrack(deviceId))
        {
            NX_DEBUG(this, "%1: minting a late audio track for device %2", id(), deviceId);
            m_tracks->addTrack(
                std::make_unique<Track>(Track{
                    .deviceId = deviceId,
                    .trackType = TrackType::audio,
                }));
        }
    }

    //< Force initializeMuxersInternal() to rebuild this device's encoder. The
    //< provider's codec cache was already invalidated by play() on the seek, so
    //< getVideoCodecParameters() now reports the new codec; createEncoders()
    //< overwrites the EOF'd encoder in place and refreshes the track's rtpmap.
    provider->setInitialized(false);

    //< Rotate the SSRCs so the peer treats the rebuilt encoders as fresh RTP
    //< streams. Reusing an SSRC makes Chrome keep the old SRTP replay window and
    //< ROC, so the encoder's reseeded sequence numbers get discarded for several
    //< seconds until it resyncs — the source of the long switch stall.
    //< initializeMuxersInternal() recreates both encoders, so rotate both; the
    //< audio call no-ops when the stream has no audio track.
    m_tracks->rotateSsrc(deviceId, TrackType::video);
    m_tracks->rotateSsrc(deviceId, TrackType::audio);

    if (!initializeMuxersInternal())
    {
        //< Couldn't build an encoder for the new codec. Leave the consumer paused;
        //< its renegotiation timer will fire and fall back to a full reconnect.
        NX_WARNING(
            this,
            "%1: failed to rebuild encoder for device %2 on codec change",
            id(),
            deviceId);
        return;
    }

    if (m_tracker && m_tracker->isStarted())
        m_tracker->sendOffer();
}

void Session::addProvider(std::shared_ptr<AbstractCameraDataProvider> provider)
{
    const auto deviceId = provider->deviceId();

    NX_MUTEX_LOCKER lock(&m_mutex);
    if (m_stopped)
        return;

    //< A device has at most one track per type, so only mint what it is missing.
    //< An in-place camera switch (setCamera) hands the previous camera's tracks
    //< to this device on purpose - their ssrc, mid and cname are what the peer
    //< already negotiated - and a second pair would go out as duplicate m-lines
    //< the peer cannot parse.
    if (provider->getVideoCodecParameters() && !m_tracks->videoTrack(deviceId))
    {
        auto video = std::make_unique<Track>(Track{
            .deviceId = deviceId,
            .trackType = TrackType::video,
        });
        m_tracks->addTrack(std::move(video));
    }

    if (provider->getAudioCodecParameters() && !m_tracks->audioTrack(deviceId))
    {
        auto audio = std::make_unique<Track>(Track{
            .deviceId = deviceId,
            .trackType = TrackType::audio,
        });
        m_tracks->addTrack(std::move(audio));
    }

    m_providers.emplace(deviceId, provider);
    provider->setDataConsumer(m_consumer.get());
}

void Session::setTranscodingParams(
    const nx::core::transcoding::Settings& transcodingSettings,
    std::optional<nx::vms::api::ResolutionData> resolution,
    std::optional<nx::vms::api::ResolutionData> resolutionWhenTranscoding)
{
    m_transcodingSettings = transcodingSettings;
    m_resolution = resolution;
    m_resolutionWhenTranscoding = resolutionWhenTranscoding;
    m_muxer = std::make_unique<Transcoder>(this);
}

bool Session::initializeMuxersInternal()
{
    // Ignore aspect ratio if user specifies the full resolution.
    if (m_resolution && m_resolution->size.width() > 0)
        m_transcodingSettings.aspectRatio = QnAspectRatio();

    bool anyAlive = false;
    bool anyFailed = false;
    for (const auto& [_, provider]: m_providers)
    {
        // A refused camera neither lives nor fails. Building it again would only repeat the probe
        // of its codec - for a live camera a wait for a keyframe, on the thread that muxes the
        // rest - and the refusal is lifted by forgetUnsupportedVideoCodec() alone.
        if (unsupportedVideoCodec(provider->deviceId()))
            continue;
        if (!provider->isInitialized() && !initializeProviderMuxer(provider))
        {
            anyFailed = true;
            continue;
        }
        if (provider->isInitialized())
            anyAlive = true;
    }
    // A camera with no stream yet neither lives nor fails, so a session of nothing but offline
    // cameras still stands - as does an empty one, which is how a session is born.
    return anyAlive || !anyFailed;
}

bool Session::initializeProviderMuxer(const AbstractCameraDataProviderPtr& provider)
{
    auto videoCodecParamaters = provider->getVideoCodecParameters();
    auto audioCodecParamaters = provider->getAudioCodecParameters();

    // A camera with no live video stream yet (offline, or still connecting) has no codec
    // params, and addProvider minted no track for it. Skip it rather than count it as failed -
    // it initializes on a later pass if its stream comes online. Mirrors the video-only audio
    // fallback below.
    if (!videoCodecParamaters && !audioCodecParamaters)
    {
        NX_DEBUG(
            this,
            "%1: device %2 has no streams yet; skipping its muxer",
            id(),
            provider->deviceId());
        return true;
    }

    const auto videoTrack = m_tracks->videoTrack(provider->deviceId());
    const auto audioTrack = m_tracks->audioTrack(provider->deviceId());

    const bool useFallbackVideo = videoTrack && videoTrack->useTranscoding;
    const bool useFallbackAudio = audioTrack && audioTrack->useTranscoding;

    if (videoCodecParamaters)
    {
        bool videoTranscodingRequired = m_transcodingSettings.isTranscodingRequired()
            || m_resolution || useFallbackVideo
            || !m_muxer->isVideoCodecSupported(videoCodecParamaters->codec_id);

        const auto transcodePolicy = m_sessionPool->transcodePolicy();
        if (transcodePolicy.disableTranscoding && videoTranscodingRequired)
        {
            NX_DEBUG(
                this,
                "%1: Video transcoding is disabled(source codec: %2) in the server settings. "
                "Feature unavailable. RTP",
                id(),
                videoCodecParamaters->codec_id);
            noteUnsupportedVideoCodec(provider->deviceId(), videoCodecParamaters->codec_id);
            return false;
        }

        if (videoTranscodingRequired)
        {
            QnFfmpegVideoTranscoder::Config config(transcodePolicy);
            config.targetCodecId = kFallbackSrtpVideoCodec;
            config.quality = nx::vms::api::StreamQuality::normal;
            config.rtpContatiner = true;

            if (m_resolution)
                config.outputResolutionLimit = m_resolution->size;
            else if (m_resolutionWhenTranscoding)
                config.outputResolutionLimit = m_resolutionWhenTranscoding->size;
            config.useMultiThreadEncode = nx::transcoding::useMultiThreadEncode(
                config.targetCodecId,
                config.outputResolutionLimit);

            config.params = suggestMediaStreamParams(config.targetCodecId, config.quality);
            if (!provider->addVideoTranscoder(config, m_transcodingSettings))
            {
                NX_DEBUG(
                    this,
                    "%1: Video codec %2 of device %3 needs a transcoder and none is available",
                    id(),
                    videoCodecParamaters->codec_id,
                    provider->deviceId());
                noteUnsupportedVideoCodec(provider->deviceId(), videoCodecParamaters->codec_id);
                return false;
            }

            NX_DEBUG(
                this,
                "%1: Video transcoding started, target codec: %2",
                id(),
                config.targetCodecId);
        }
    }

    bool hasAudio = audioCodecParamaters != nullptr;
    if (hasAudio)
    {
        bool audioTranscodingRequired =
            useFallbackAudio || !m_muxer->isAudioCodecSupported(audioCodecParamaters->codec_id);
        if (audioTranscodingRequired)
        {
            if (provider->addAudioTranscoder(kFallbackSrtpAudioCodec))
            {
                NX_DEBUG(
                    this,
                    "%1: Audio transcoding started, target codec: %2",
                    id(),
                    kFallbackSrtpAudioCodec);
            }
            else
            {
                // Video is the product, audio is garnish: a transcoder that
                // cannot open must not take the session down with it. Drop
                // the track and serve video-only (VMS-191).
                NX_WARNING(
                    this,
                    "%1: Cannot open audio transcoder for codec %2, serving video-only",
                    id(),
                    audioCodecParamaters->codec_id);
                hasAudio = false;
            }
        }
    }

    // On success the provider reports the transcoder's output params, so it
    // must be re-read rather than reusing the source ones read above.
    if (!m_muxer->createEncoders(
            provider->deviceId(),
            provider->getVideoCodecParameters(),
            hasAudio ? provider->getAudioCodecParameters() : nullptr))
    {
        NX_DEBUG(this, "%1: Cannot create encoders for device %2", id(), provider->deviceId());
        if (videoCodecParamaters)
            noteUnsupportedVideoCodec(provider->deviceId(), videoCodecParamaters->codec_id);
        return false;
    }
    provider->setInitialized(true);
    return true;
}

void Session::noteUnsupportedVideoCodec(const nx::Uuid& deviceId, AVCodecID codecId)
{
    bool isNew = false;
    {
        NX_MUTEX_LOCKER lock(&m_mutex);
        isNew = m_unsupportedVideoCodecs.insert_or_assign(deviceId, codecId).second;
    }
    if (isNew)
    {
        NX_WARNING(
            this,
            "%1: Camera %2 stays in the session without video: codec %3 cannot reach this peer",
            id(),
            deviceId,
            avcodec_get_name(codecId));
    }
}

std::map<nx::Uuid, AVCodecID> Session::unsupportedVideoCodecs() const
{
    NX_MUTEX_LOCKER lock(&m_mutex);
    return m_unsupportedVideoCodecs;
}

std::optional<AVCodecID> Session::unsupportedVideoCodec(const nx::Uuid& deviceId) const
{
    NX_MUTEX_LOCKER lock(&m_mutex);
    const auto it = m_unsupportedVideoCodecs.find(deviceId);
    return it == m_unsupportedVideoCodecs.end() ? std::nullopt : std::make_optional(it->second);
}

void Session::forgetUnsupportedVideoCodec(const nx::Uuid& deviceId)
{
    NX_MUTEX_LOCKER lock(&m_mutex);
    m_unsupportedVideoCodecs.erase(deviceId);
    // The refusal came from the fallback; the codec the camera has now is offered as it is.
    for (auto& [_, track]: m_tracks->tracks())
    {
        if (track->deviceId != deviceId)
            continue;
        track->offerState = TrackState::active;
        track->useTranscoding = false;
    }
}

void Session::setReader(std::shared_ptr<Reader> reader)
{
    m_reader = std::move(reader);
}

DataPeer* Session::dataPeer()
{
    if (m_purpose != Purpose::data)
        return nullptr;
    return static_cast<DataPeer*>(m_transceiver.get());
}

void Session::setDataPeerHandlers(DataPeerHandlers handlers)
{
    if (auto* peer = dataPeer(); NX_ASSERT(peer, "Session %1 is not a data session", id()))
        peer->setHandlers(std::move(handlers));
}

Streamer* Session::streamer()
{
    if (m_purpose != Purpose::send)
        return nullptr;
    return static_cast<Streamer*>(m_transceiver.get());
}

void Session::setTalkHandlers(TalkHandlers handlers)
{
    if (auto* s = streamer(); NX_ASSERT(s, "Session %1 is not a send session", id()))
        s->setTalkHandlers(std::move(handlers));
}

void Session::setAddress(const nx::network::SocketAddress& address)
{
    m_localAddress = address;
}

std::shared_ptr<nx::metric::Storage> Session::metrics() const
{
    return systemContext()->metrics();
}

std::optional<SessionDescription> Session::describe() const
{
    NX_MUTEX_LOCKER lk(&m_mutex);
    if (isLocked())
        return std::nullopt;

    return describeUnsafe();
}

SessionDescription Session::describeUnsafe() const
{
    return SessionDescription{
        .id = m_localUfrag,
        .remoteUfrag = m_sdpParseResult.iceUfrag,
        .localPwd = m_localPwd,
        .remotePwd = m_sdpParseResult.icePwd,
        .tieBreaker = m_tieBreaker,
        .dtls = m_dtls};
}

std::string Session::dataChannelSdp(uint16_t port, bool bundleOnly, int maxMessageSize) const
{
#define ENDL "\r\n";
    std::string sdp;
    sdp += "m=application " + std::to_string(port) + " UDP/DTLS/SCTP webrtc-datachannel" ENDL;
    sdp += "a=mid:" + std::to_string(kDataChannelMid) + ENDL;
    // Port in SCTP level.
    sdp += "a=sctp-port:" + std::to_string(kDefaultSctpPort) + ENDL;
    // SCTP max message size: the largest message the peer may send US (our
    // receive path reassembles fragments regardless, but a spec-conforming
    // peer caps its sends at this).
    sdp += "a=max-message-size:" + std::to_string(maxMessageSize) + ENDL;
    if (bundleOnly)
    {
        // Can be only bundled with another tracks.
        sdp += "a=bundle-only" ENDL;
    }
    /* https://datatracker.ietf.org/doc/html/rfc3264#section-5.1
     * If the offerer wishes to only send media on a stream to its peer, it
     * MUST mark the stream as sendonly with the "a=sendonly" attribute.
     * */
    sdp += "a=sendrecv" ENDL;
    /* 'actpass' due to bug in old Chromium. Actually, for incoming connection, should be
     * 'passive': https://datatracker.ietf.org/doc/html/rfc4145#section-4 */
    sdp += "a=setup:actpass" ENDL;
    return sdp;
}

std::string Session::constructFingerprint() const
{
    ssl::CertificateView certificateView(m_pem.certificate().x509());
    auto fingerprint = certificateView.sha256();
    std::string result = "a=fingerprint:sha-256 ";
    constexpr char hexDigits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < fingerprint.size(); ++i)
    {
        if (i != 0)
            result += ':';
        result += hexDigits[(fingerprint[i] >> 4) & 0xf];
        result += hexDigits[fingerprint[i] & 0xf];
    }
    return result;
}

std::string Session::constructSdp()
{
    auto tracks = m_tracks->allTracks();
    // A track whose encoder could not be built has no m-line to describe; listing its mid in the
    // bundle with none behind it makes the peer turn down the whole offer.
    std::erase_if(
        tracks,
        [this](const Track& track)
        {
            return track.trackType == TrackType::video ? !m_muxer->videoEncoder(track.deviceId)
                                                       : !m_muxer->audioEncoder(track.deviceId);
        });

    std::string sdp;
    std::string localAddress = (m_localAddress.address.isPureIpV6() ? "IP6 " : "IP4 ")
        + m_localAddress.address.toString();
#define ENDL "\r\n";
    sdp += "v=0" ENDL;
    sdp += "o=- 4833640938783375127 " + std::to_string(++m_version) + " IN " + localAddress + ENDL;
    sdp += "s=-" ENDL;
    sdp += "c=IN " + localAddress + ENDL;
    sdp += "t=0 0" ENDL;
    // Lite agent implementation.
    sdp += "a=ice-lite" ENDL;
    // Fingerprint of certificate which will be used for DTLS.
    sdp += constructFingerprint() + ENDL;
    // The group lists the mids in m-line order: the data channel leads (see below).
    sdp += "a=group:BUNDLE " + std::to_string(kDataChannelMid);
    for (const auto& track: tracks)
        sdp += " " + std::to_string(track.mid);
    sdp += "" ENDL;
    // Inform that ice candidates will be sent later.
    sdp += "a=ice-options:trickle" ENDL;
    // Probably unused.
    sdp += "a=msid-semantic: WMS *" ENDL;

    sdp += "a=ice-ufrag:" + m_localUfrag + ENDL;
    sdp += "a=ice-pwd:" + m_localPwd + ENDL;

    auto port = m_localAddress.port;

    //< The data channel m-line leads at index 0 in every offer: it is the one m-line
    //< every session always has (tracks can be minted later - an offline camera joins
    //< with none and gains them via renegotiation), and an existing m-line must never
    //< change position across offers. It carries the bundle's real port; the tracks
    //< follow, bundle-only.
    sdp += dataChannelSdp(port, /*bundleOnly*/ false, kSctpMaxMessageSizeData);
    for (const auto& track: tracks)
    {
        sdp += m_tracks->getSdpForTrack(&track, /*port*/ 9);
        sdp += std::string("a=bundle-only") + ENDL;
    }
    return sdp;
}

AnswerResult Session::processSdpAnswer(const std::string& sdp)
{
    m_sdpParseResult = SdpParseResult::parse(sdp);
    NX_DEBUG(
        this,
        "%1: Sdp parsed, got remote: %2/%3",
        id(),
        m_sdpParseResult.iceUfrag,
        m_sdpParseResult.icePwd);

    if (m_sdpParseResult.fingerprint.type == Fingerprint::Type::none)
    {
        NX_DEBUG(this, "%1: No DTLS fingerprint in answer", id());
        return AnswerResult::failed;
    }
    m_dtls->setFingerprint(m_sdpParseResult.fingerprint);

    if (m_sdpParseResult.tracksState[kDataChannelMid] != TrackState::active)
    {
        NX_DEBUG(
            this,
            "%1: Tracks in offer and answer does not match (application track doesn't exists)",
            id());
        return AnswerResult::failed;
    }

    std::set<nx::Uuid> movedToFallback;
    for (auto& [_, track]: m_tracks->tracks())
    {
        const auto state = m_sdpParseResult.tracksState[track->mid];
        track->answerState = state;
        if (state == TrackState::inactive && track->offerState == TrackState::active)
        {
            if (track->useTranscoding)
                return AnswerResult::failed; //< Still failed after traying fallback codec
            movedToFallback.insert(track->deviceId);
            track->useTranscoding = true;
            if (auto it = m_providers.find(nx::Uuid(track->deviceId)); it != m_providers.end())
                it->second->setInitialized(false); //< Allow to reinit provider.
        }
    }

    const bool anyServed = initializeMuxersInternal();

    // A camera the fallback could not be built for keeps its m-lines, as the peer left them:
    // inactive. Offering them again would only be turned down again, and that second rejection
    // is what fails a session.
    for (const auto& [deviceId, codecId]: unsupportedVideoCodecs())
    {
        movedToFallback.erase(deviceId);
        for (auto& [ssrc, track]: m_tracks->tracks())
        {
            if (track->deviceId == deviceId)
                track->offerState = TrackState::inactive;
        }
    }

    if (!anyServed)
    {
        // No camera is left to serve; the tracker names the ones the peer refused.
        return unsupportedVideoCodecs().empty() ? AnswerResult::failed
                                                : AnswerResult::unsupportedCodec;
    }

    return movedToFallback.empty() ? AnswerResult::success : AnswerResult::again;
}

AnswerResult Session::setFallbackCodecs()
{
    for (auto& [_, track]: m_tracks->tracks())
    {
        if (track->offerState == TrackState::inactive)
            continue;
        if (track->useTranscoding)
            return AnswerResult::failed; //< Still failed after traying fallback codec
        track->useTranscoding = true;
        track->answerState = TrackState::inactive;
        if (auto it = m_providers.find(nx::Uuid(track->deviceId)); it != m_providers.end())
            it->second->setInitialized(false); //< Allow to reinit provider.
    }

    bool status = initializeMuxersInternal();
    return status ? AnswerResult::again : AnswerResult::failed;
}

const std::vector<IceCandidate> Session::getIceCandidates() const
{
    NX_MUTEX_LOCKER lk(&m_mutex);
    return m_iceCandidates;
}

bool Session::lock()
{
    NX_MUTEX_LOCKER lk(&m_mutex);
    if (isLocked())
        return false;

    NX_DEBUG(this, "Locked session %1", id());
    m_locked = true;
    // Repurpose the reap clock as the streaming liveness clock: a locked session
    // is now reaped on inbound silence, not idleness. Start fresh so a peer that
    // dies right after locking still gets the full timeout before being reaped.
    m_timer.restart();
    return true;
}

void Session::setActiveIce(Ice* ice)
{
    m_activeIce = ice;
}

void Session::stopUnsafe()
{
    const auto stopIce = [](Ice* ice)
    {
        if (ice)
            ice->stopUnsafe();
    };

    // The active ice first: its stopUnsafe() stops the consumer chain, unlocks the session and
    // clears m_activeIce. It may be a pool-owned IceTcp, reachable only through this pointer.
    stopIce(m_activeIce);

    // The srflx probe next: its socket shares this thread, so the reset is a direct call, and
    // once it is gone nothing mutates the ice members below from another thread.
    m_srflx.reset();

    for (const auto& udpIce: m_udpIces)
        stopIce(udpIce.get());
    stopIce(m_punchedUdpIce.get());
    stopIce(m_relayIce.get());

    m_relayFallbackTimer.pleaseStopSync();
    stopAllProviders();
    m_reader.reset();
    m_quiesced = true;
}

void Session::stop(std::shared_ptr<Session> session)
{
    auto* const aioThread = session->m_aioThread;
    aioThread->post(
        nullptr,
        [session = std::move(session)]()
        {
            session->stopUnsafe();
        }); //< The lambda dies with its reference; if it is the last one, ~Session runs here.
}

void Session::unlock()
{
    NX_MUTEX_LOCKER lk(&m_mutex);
    NX_DEBUG(this, "Unlocked session %1", id());
    m_locked = false;
    m_timer.restart();
}

void Session::onInboundActivity()
{
    NX_MUTEX_LOCKER lk(&m_mutex);
    if (m_locked)
        m_timer.restart();
}

bool Session::isStreaming() const
{
    NX_MUTEX_LOCKER lk(&m_mutex);
    return m_locked;
}

bool Session::hasExpired(std::chrono::milliseconds timeout) const
{
    NX_MUTEX_LOCKER lk(&m_mutex);
    // One clock for both cases: while idle nothing refreshes m_timer so it ages
    // out (idle reap); while streaming, onInboundActivity() refreshes it, so it
    // ages out only when the peer goes silent. SessionPool distinguishes the two
    // via isStreaming() for logging.
    return m_timer.isValid() && m_timer.hasExpired(timeout);
}

void Session::addIceCandidate(const IceCandidate& iceCandidate)
{
    NX_MUTEX_LOCKER lk(&m_mutex);
    addIceCandidateUnsafe(iceCandidate);
}

void Session::addIceCandidateUnsafe(const IceCandidate& iceCandidate)
{
    int index = IceCandidate::kIceMinIndex;
    int priority = IceCandidate::kIceMaxPriority;
    if (!m_iceCandidates.empty())
    {
        index = m_iceCandidates.back().foundation + 1;
        priority = m_iceCandidates.back().priority - 1;
    }
    m_iceCandidates.push_back(iceCandidate);
    m_iceCandidates.back().foundation = index;
    m_iceCandidates.back().priority = priority;

    if (m_tracker)
        m_tracker->onIceCandidate(m_iceCandidates.back());
}

nx::network::aio::AbstractAioThread* Session::aioThread() const
{
    return m_aioThread;
}

void Session::releaseTracker()
{
    auto tracker = m_tracker;
    {
        NX_MUTEX_LOCKER lk(&m_mutex);
        m_tracker.reset();
    }
}

std::shared_ptr<AbstractCameraDataProvider> Session::provider(nx::Uuid deviceId) const
{
    NX_MUTEX_LOCKER lock(&m_mutex);
    auto it = m_providers.find(deviceId);
    return it != m_providers.end() ? it->second : nullptr;
}

std::vector<std::shared_ptr<AbstractCameraDataProvider>> Session::allProviders() const
{
    NX_MUTEX_LOCKER lk(&m_mutex);
    std::vector<std::shared_ptr<AbstractCameraDataProvider>> result;
    for (const auto& [_, p]: m_providers)
        result.push_back(p);
    return result;
}

void Session::stopAllProviders()
{
    decltype(m_providers) providers;
    {
        NX_MUTEX_LOCKER lk(&m_mutex);
        std::swap(providers, m_providers);
        m_stopped = true;
    }
    for (const auto& [_, provider]: providers)
        provider->stop();
}

} // namespace nx::webrtc
