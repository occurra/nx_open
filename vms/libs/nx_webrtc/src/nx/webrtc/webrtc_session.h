// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <deque>
#include <functional>
#include <optional>
#include <set>

#include <nx/network/aio/timer.h>
#include <nx/network/ssl/certificate.h>
#include <nx/network/ssl/context.h>
#include <nx/utils/move_only_func.h>
#include <nx/vms/api/data/resolution_data.h>
#include <nx/vms/common/system_context_aware.h>

#include "abstract_camera_data_provider.h"
#include "webrtc_consumer.h"
#include "webrtc_demuxer.h"
#include "webrtc_ice.h"
#include "webrtc_reader.h"
#include "webrtc_tracks.h"
#include "webrtc_transcoder.h"

namespace nx::webrtc {

static const int kDataChannelMid = 128;

enum AnswerResult
{
    success = 0,
    again,
    noop,
    failed,
    /** The peer rejected a video codec and the server cannot transcode it. */
    unsupportedCodec
};

class SessionPool;
class Tracker;
class AbstractIceDelegate;
class DataPeer;
struct DataPeerHandlers;
struct TalkHandlers;
class Streamer;
class IceUdp;
class IceRelay;
class Srflx;
class Session;
class ConnectionProcessor;

class NX_WEBRTC_API Session: protected nx::vms::common::SystemContextAware
{
public:
    static std::string generateLocalUfrag();

    Session(
        nx::vms::common::SystemContext* context,
        SessionPool* sessionPool,
        createDataProviderFactory providerFactory,
        const std::string& localUfrag,
        const nx::network::SocketAddress& address,
        const SessionConfig& config,
        const std::vector<nx::network::SocketAddress>& m_stunServers,
        Purpose purpose,
        nx::network::aio::AbstractAioThread* aioThread = nullptr);
    ~Session();

    void start(const QList<nx::network::SocketAddress>& addresses);
    void setAddress(const nx::network::SocketAddress& address);
    std::string constructSdp();
    /** The version of the offer constructSdp() built last, as its o= line carries it. */
    int offerVersion() const { return m_version; }
    AnswerResult processSdpAnswer(const std::string& sdp);
    AnswerResult setFallbackCodecs();
    /** The cameras the session refuses, each with the video codec it was refused at. */
    std::map<nx::Uuid, AVCodecID> unsupportedVideoCodecs() const;
    /** The video codec this camera is refused at, if it is. */
    std::optional<AVCodecID> unsupportedVideoCodec(const nx::Uuid& deviceId) const;
    /** Lifts the camera's refusal, so its next muxer build offers the codec it has now. */
    void forgetUnsupportedVideoCodec(const nx::Uuid& deviceId);
    Purpose purpose() const { return m_purpose; }
    std::string id() const { return m_localUfrag; }
    std::optional<SessionDescription> describe() const;
    const std::vector<IceCandidate> getIceCandidates() const;

    const nx::network::ssl::Pem& getPem() const { return m_pem; }
    void addIceCandidate(const IceCandidate& iceCandidate);
    std::shared_ptr<Tracker> tracker() { return m_tracker; }
    void releaseTracker();

    /// The AIO thread this session runs on: the one its signalling socket
    /// arrived on, carried through the tracker. The Consumer and every ice the
    /// session creates are bound to it when they are constructed, so nothing
    /// needs rebinding once an ice wins the DTLS handshake. Valid from
    /// construction on: the tracker is built before anything that asks.
    nx::network::aio::AbstractAioThread* aioThread() const;

    // Should be called in `unlocked` state.
    void setReader(std::shared_ptr<Reader> reader);

    SessionPool* sessionPool() const { return m_sessionPool; }
    void setTranscodingParams(
        const nx::core::transcoding::Settings& transcodingParams,
        std::optional<nx::vms::api::ResolutionData> resolution = std::nullopt,
        std::optional<nx::vms::api::ResolutionData> resolutionWhenTranscoding = std::nullopt);

    /** Starts a camera's live stream so its first keyframe is ready before a codec probe waits on
     *  it. Installed by the media server, which owns the device pool. */
    using WarmUpProvider = std::function<void(nx::Uuid, nx::vms::api::StreamIndex)>;

    /** Installs the warm-up hook. Invoked for every camera setCameras adds, so a batch of cameras
     *  warms up in parallel instead of one blocking codec probe at a time. */
    void setWarmUpProvider(WarmUpProvider warmUp) { m_warmUpProvider = std::move(warmUp); }
    std::shared_ptr<nx::metric::Storage> metrics() const;
    Reader* reader() { return m_reader.get(); }
    Demuxer* demuxer() { return m_demuxer.get(); }
    Tracks* tracks() { return m_tracks.get(); }
    Transcoder* muxer() { return m_muxer.get(); }
    std::shared_ptr<AbstractCameraDataProvider> provider(nx::Uuid deviceId) const;
    std::vector<std::shared_ptr<AbstractCameraDataProvider>> allProviders() const;
    Consumer* consumer() { return m_consumer.get(); }
    AbstractIceDelegate* transceiver() { return m_transceiver.get(); }

    /// The datachannel endpoint of a Purpose::data session; null otherwise.
    DataPeer* dataPeer();

    /// Install the DataPeer callbacks (Purpose::data sessions only). Must be
    /// called before tracker()->start() - see DataPeer::setHandlers.
    void setDataPeerHandlers(DataPeerHandlers handlers);

    /// The video streamer of a Purpose::send session; null otherwise.
    Streamer* streamer();

    /// Install talk-back callbacks so the browser's microphone rides this video
    /// session's data channel (Purpose::send sessions only). See TalkHandlers.
    void setTalkHandlers(TalkHandlers handlers);
    const SessionConfig& config() const { return m_config; }
    void gotIceFromTracker(const IceCandidate& iceCandidate);
    void stopAllProviders();

    /// Called when any non-relay ICE transport passes its STUN binding check,
    /// i.e. a direct path works — cancels the pending relay fallback.
    void onDirectConnectivityEstablished();

    /// Called when a binding success response to one of our own connectivity
    /// checks arrives while that ice is still in the binding stage: the direct
    /// path works end-to-end and the peer's authenticated check is likely in
    /// flight. Extends the relay fallback timer (bounded) so the deferred
    /// private candidates and the relay do not race a direct handshake that is
    /// about to succeed. The response is unauthenticated, so it only ever
    /// delays the fallback — it never cancels it.
    void onDirectPathProbeSucceeded();

    AbstractCameraDataProviderPtr createProvider(
        nx::Uuid deviceId,
        std::optional<std::chrono::milliseconds> positionMs,
        nx::vms::api::StreamIndex stream,
        std::optional<float> speedOpt);

    /**
     * @return false, if session already locked.
     * NOTE: Cancel session deletion timer for keepAliveTimeoutSec and lock the session for other
     * ices.
     */
    bool lock();
    /**
     * NOTE: Starts session deletion timer for keepAliveTimeoutSec if no refs found.
     */
    void unlock();
    /**
     * Registers the ice whose DTLS handshake locked this session - the only ice whose callbacks
     * can reach it through their weak reference - so teardown can sever that link. The ice calls
     * it with itself when it locks the session and with null from its stopUnsafe(); both run on
     * aioThread(), the only thread that ever reads the value back (see detachActiveIce()), so the
     * field needs no lock.
     */
    void setActiveIce(Ice* ice);

    /** The pair a locked session runs on. */
    struct ConnectedPath
    {
        IceCandidate::Filter type = IceCandidate::Filter::None;
        nx::network::SocketAddress local;
        nx::network::SocketAddress remote;
    };
    /** The pair this session runs on; nothing while unlocked. Thread-safe. */
    std::optional<ConnectedPath> connectedPath() const;
    /**
     * Arranges the session's safe destruction. The pool calls it with the reference it removed
     * from its map. A task posted - not dispatched: the callers are typically beneath ice
     * callback frames on this very thread (Receiver::stop -> drop), and the teardown must wait
     * for that stack to unwind - to the session's aioThread() carries the reference, runs
     * stopUnsafe() quiescing every ice, timer and provider, and then releases it. Every ice that
     * can lock the session lives on that thread (an IceTcp migrates there before the DTLS
     * handshake that locks it), so at the task's top level no ice callback is on the stack
     * holding a transient reference; and because nothing thread-affine is left running
     * afterwards, whichever reference then dies last - the task's, or a transient one in a
     * signalling handler on some other thread - runs a ~Session that only frees memory.
     * Destroying the session directly on the pool's thread instead races an in-flight ice
     * callback whose transient reference would become the last one and run ~Session beneath the
     * very DataChannel it frees. onStopped, if given, runs on that thread once the task has
     * released its reference; SessionPool::stop() waits for it so no session outlives the pool.
     */
    static void stop(
        std::shared_ptr<Session> session, nx::MoveOnlyFunc<void()> onStopped = nullptr);
    bool hasExpired(std::chrono::milliseconds timeout) const;

    /**
     * Mark that an inbound packet arrived from the peer. While the session is
     * locked (streaming) this refreshes the liveness clock hasExpired() checks,
     * so a peer that silently vanishes (a hung browser tab, a dropped relay
     * path with no consent checks) is reaped after the timeout instead of
     * holding the camera's provider forever.
     */
    void onInboundActivity();

    /**
     * Whether the session is locked (a peer has completed DTLS and is
     * streaming). SessionPool uses it to distinguish a peer-silence reap of a
     * live stream from an ordinary idle reap when logging.
     */
    bool isStreaming() const;

    bool removeProvider(nx::Uuid deviceId);

    /** One camera of a setCameras() request: the device and the stream to open it at. */
    struct CameraRequest
    {
        nx::Uuid deviceId;
        nx::vms::api::StreamIndex stream = nx::vms::api::StreamIndex::primary;
    };

    /**
     * Reconcile the session's providers to exactly cameras. A camera that leaves is paired with
     * one that arrives and its m-line is retargeted in place - same mid, ssrc and RTP timeline,
     * so a same-codec swap never reaches the peer as an offer and Chrome sees one continuous
     * stream that merely changes content (the decoder takes the new SPS/PPS in stride). Only
     * fresh and parked m-lines need an offer, sent once for the whole diff: extra arrivals and
     * the partners of a leaving camera with no video track get a fresh m-line, extra departures
     * are parked inactive. A codec change on a retargeted m-line is caught downstream and
     * re-offered by beginCodecRenegotiation, exactly as a mid-stream codec change is. positionMs
     * positions the cameras this command brings in (archive), or opens them live when unset.
     * @param rebase Client-chosen RTP-timeline rebase, exactly as for setCamera and seek (see
     *     Consumer::RtpRebase). Applied to every retargeted camera so the player can discard the
     *     previous camera's frames by comparing frame RTP against the base it picked.
     * @return The devices switched in by an in-place retarget, which send no offer and are
     *     acknowledged directly, the way setCamera is. Empty while an offer is in flight: the
     *     set is kept, the latest one wins, and the answer applies it.
     */
    std::vector<nx::Uuid> setCameras(
        const std::vector<CameraRequest>& cameras,
        std::optional<std::chrono::milliseconds> positionMs,
        std::optional<Consumer::RtpRebase> rebase = std::nullopt);

    /**
     * Marks that a renegotiation offer is on the wire; the Tracker calls this from sendOffer().
     * While one is in flight, createProvider/removeProvider queue the change instead of mutating
     * the track set, so an answer never has to reconcile against a track added mid-round.
     */
    void markOfferSent();

    /**
     * The Tracker calls this on a clean answer (the round is done): apply any add/remove commands
     * that queued during the round, then the camera set asked for last, and send one coalesced
     * offer for the resulting state.
     */
    void onOfferAnswered();

    /**
     * True while deviceId's switch offer is on the wire but unanswered. The peer does not decode
     * a just-activated m-line until it applies the answer, so a keyframe sent before then is
     * dropped; the Consumer holds the device's media until this turns false. This is why a live
     * switch glitches intermittently but an archive one never does - an archive prime reads from
     * disk and always lands after the answer, while a live prime is instant and races ahead of it.
     */
    bool offerInProgress(nx::Uuid deviceId) const;

    /**
     * Replace the camera this session streams, without touching the SDP: stops
     * the provider that is streaming, hands its negotiated tracks and RTP
     * encoders to deviceId, and starts the new provider on them. The peer sees
     * one uninterrupted stream - same m-lines, payload types, ssrcs and RTP
     * timeline - so a same-codec switch needs no offer/answer at all. A camera
     * whose codec the encoder cannot take fails on its first packet and goes
     * through the ordinary codec renegotiation.
     *
     * For single-camera sessions only: the camera to replace is the one
     * streaming, so callers never track it. A session holding several providers
     * has no such camera and must use createProvider/removeProvider instead.
     * @param positionMs Archive position for the new camera, or unset for live.
     * @param keyframeOnly Land at the nearest preceding key frame instead of
     *     pre-rolling to positionMs, exactly as for a seek. Changing camera is
     *     its own intent, so the caller states it rather than inheriting the
     *     precision of whatever seek came last.
     * @param rebase Client-chosen RTP-timeline rebase, exactly as for a seek
     *     (see Consumer::RtpRebase): the new camera's packets carry the seq as
     *     their marker and their output timeline restarts at rtpBase90k, so the
     *     player can hold the previous camera's picture until a frame it can
     *     prove came from this switch arrives. Omit for legacy clients.
     * @return The provider now streaming (the existing one when deviceId is
     *     already on air), or null if this session is not streaming exactly one
     *     camera or the new provider could not be created.
     */
    AbstractCameraDataProviderPtr setCamera(
        nx::Uuid deviceId,
        std::optional<std::chrono::milliseconds> positionMs,
        nx::vms::api::StreamIndex stream,
        bool keyframeOnly = false,
        std::optional<Consumer::RtpRebase> rebase = std::nullopt);

    /// Rebuild the RTP encoder for a device against its current (changed) codec
    /// and push a fresh SDP offer to the peer. Invoked by the Tracker on its own
    /// aio thread after the Consumer detects a codec change. The Consumer keeps
    /// media paused until the answer arrives, so this runs with the muxer idle.
    /** True when the device already has a track matching the packet's type - video
     *  for a video packet, audio for an audio one; other types need no track. */
    bool hasTrackForMedia(const QnConstAbstractMediaDataPtr& media);

    void beginCodecRenegotiation(nx::Uuid deviceId);

    /**
     * Keeps a mid-stream audio codec change off the wire: makes the provider
     * transcode the new source codec into the one the audio track already
     * negotiated, so the payload type, the SDP and the SSRC all stay as they
     * are and streaming continues without an offer/answer round trip. Runs on
     * the muxing thread, synchronously with the rejected packet.
     * @param media Packet the encoder rejected. Its context seeds the
     *     transcoder input; the packet itself is not recoverable and the caller
     *     drops it.
     * @return True when the encoder can resume, false when the change needs a
     *     real renegotiation (video, natively receivable codec, no transcoder).
     */
    bool adaptToAudioCodecChange(nx::Uuid deviceId, const QnConstAbstractMediaDataPtr& media);

    std::string idForToStringFromPtr() const;

private:
    std::string dataChannelSdp(
        uint16_t port,
        bool bundleOnly,
        int maxMessageSize = kSctpMaxMessageSize) const;
    std::string constructFingerprint() const;
    /** True while the session can still serve somebody: a camera the muxer cannot be built for
     *  is skipped, not fatal, and only a session where every camera failed is. */
    bool initializeMuxersInternal();
    /** Builds one camera's muxer, transcoding where the peer or the settings ask for it. False
     *  when this camera cannot be served; the others are unaffected. */
    bool initializeProviderMuxer(const AbstractCameraDataProviderPtr& provider);
    /** Refuses the camera: its video cannot reach this peer at the codec it has. */
    void noteUnsupportedVideoCodec(const nx::Uuid& deviceId, AVCodecID codecId);
    void addProvider(std::shared_ptr<AbstractCameraDataProvider> provider);
    /** Creates a provider and its track without emitting an offer - the caller sends one offer
     *  after applying its batch of changes. */
    AbstractCameraDataProviderPtr createProviderNow(
        nx::Uuid deviceId,
        std::optional<std::chrono::milliseconds> positionMs,
        nx::vms::api::StreamIndex stream,
        std::optional<float> speedOpt);
    /**
     * Removes a provider and deactivates its tracks without emitting an offer. Returns true when
     * a track was deactivated - a change the next offer must carry. A device with no tracks
     * changes nothing the peer sees.
     */
    bool removeProviderNow(nx::Uuid deviceId);
    /** Hands fromDeviceId's negotiated tracks and encoders to toDeviceId in place (same mid, ssrc
     *  and RTP timeline) and starts toDeviceId's provider, without emitting an offer. The shared
     *  core of setCamera and a setCameras retarget pair. Returns the new provider, or null when
     *  fromDeviceId is not streaming or the new provider cannot be built. */
    AbstractCameraDataProviderPtr retargetProviderNow(
        nx::Uuid fromDeviceId,
        nx::Uuid toDeviceId,
        std::optional<std::chrono::milliseconds> positionMs,
        nx::vms::api::StreamIndex stream,
        bool keyframeOnly,
        std::optional<Consumer::RtpRebase> rebase);
    /** Diffs the providers against cameras and applies the result - retarget pairs, fresh
     *  m-lines, parked departures - without emitting an offer; sets *outTopologyChanged when the
     *  SDP needs one. Returns the devices switched in by an in-place retarget. */
    std::vector<nx::Uuid> applyCameraSet(
        const std::vector<CameraRequest>& cameras,
        std::optional<std::chrono::milliseconds> positionMs,
        std::optional<Consumer::RtpRebase> rebase,
        bool* outTopologyChanged);
    /** Applies every add/remove queued during the finished round and the camera set kept for
     *  it, then emits one offer. */
    void drainPendingCommands();
    /**
     * Quiesces the session: stops every ice (the active one first - it stops the consumer chain
     * and unlocks the session), the srflx probe, the relay fallback timer, the providers and the
     * reader. Runs on aioThread(), at the top level of the task stop() posts, so nothing this
     * stops can be on the stack. Afterwards ~Session has no thread-affine work left and is safe
     * on any thread.
     */
    void stopUnsafe();
    SessionDescription describeUnsafe() const;
    void armRelayFallbackUnsafe(); ///< Start the fallback timer once. m_mutex held.
    void startRelayFallbackTimerUnsafe(); ///< (Re)start the fallback timer for one period.
    void onRelayFallbackTimeout(); ///< Fallback timer body: extend, or release deferred + relay.
    void startRelayNow(); ///< Fetch creds + startRelay. Idempotent.
    void addIceCandidateUnsafe(const IceCandidate& iceCandidate);

    /**
     * Split-horizon classification: releases the deferred private-scope host
     * candidates once any of the peer's reflexive IPs equals ours (same NAT).
     * m_mutex held.
     */
    void classifyPeerUnsafe();
    void advertiseDeferredHostCandidatesUnsafe(const std::string& reason);

private:
    mutable nx::Mutex m_mutex;

    Purpose m_purpose = Purpose::send;
    std::unique_ptr<Tracks> m_tracks;
    std::unique_ptr<AbstractIceDelegate> m_transceiver;
    std::map<nx::Uuid, std::shared_ptr<AbstractCameraDataProvider>>
        m_providers; //< Streaming side. Can be null.
    /** Devices whose video cannot reach this peer - a codec the peer turned down or the muxer
     *  cannot carry, with no transcoder to change it - each with the codec it was refused at.
     * Written while an answer is processed and read by the consumer's thread, hence under m_mutex.
     * A refusal stands until the camera leaves the session or starts sending another codec. */
    std::map<nx::Uuid, AVCodecID> m_unsupportedVideoCodecs;

    /** A device add/remove that arrived while a renegotiation offer was in flight. */
    struct PendingCommand
    {
        enum class Op
        {
            add,
            remove
        };
        Op op = Op::add;
        nx::Uuid deviceId;
        std::optional<std::chrono::milliseconds> positionMs;
        nx::vms::api::StreamIndex stream = nx::vms::api::StreamIndex::primary;
        std::optional<float> speedOpt;
    };
    /** True from the moment an offer is sent until its clean answer; guards one offer/answer
     *  round at a time so rapid adds cannot put multiple offers on the wire. */
    bool m_offerInProgress = false;
    /** Adds/removes deferred while m_offerInProgress; drained on the answer. */
    std::deque<PendingCommand> m_pendingCommands;
    /** A whole camera set asked for while m_offerInProgress; the latest one replaces it. */
    struct PendingCameraSet
    {
        std::vector<CameraRequest> cameras;
        std::optional<std::chrono::milliseconds> positionMs;
        std::optional<Consumer::RtpRebase> rebase;
    };
    std::optional<PendingCameraSet> m_pendingCameraSet;
    /** Devices created since the last offer went out; markOfferSent() moves them into
     *  m_devicesAwaitingAnswer, so only devices an offer actually carried get held. */
    std::vector<nx::Uuid> m_addedSinceOffer;
    /** Devices whose in-flight offer is unanswered; their media is held until the answer.
     *  Written on the signaling thread, read on the muxing thread - guarded by m_mutex. */
    std::set<nx::Uuid> m_devicesAwaitingAnswer;
    std::unique_ptr<Transcoder> m_muxer; //< Streaming side. Can be null.
    std::shared_ptr<Consumer> m_consumer; //< Streaming side. Can be null.
    std::unique_ptr<Demuxer> m_demuxer; //< Receiving side. Can be null.
    std::shared_ptr<Reader> m_reader; //< Receiving side. Can be null.
    const std::string m_localUfrag;
    std::string m_localPwd;
    std::uint64_t m_tieBreaker = 0; ///< Stable per-session ICE tie-breaker.
    SdpParseResult m_sdpParseResult;
    nx::network::SocketAddress m_localAddress;
    nx::network::ssl::Pem m_pem;

    /* Note that Session pointer is shared between Tracker, SessionPool, and one Ice
     * (which locked this Session first).
     * Session life-time comprises 2 state: locked and unlocked.
     * Session's state change is provided by methods SessionPool::lock() and SessionPool::unlock().
     * In 'unlocked' state, Session should be used only from Tracker.
     * In 'locked' state, Session should be used only from Ice.
     * But, the fields 'm_tracker', 'm_iceCandidates' and 'm_dtls' can be used in any state.
     * So, access to these fields should be guarded by this mutex.
     * */
    std::shared_ptr<Tracker> m_tracker;
    /// Cached tracker thread - see aioThread(). Held separately because
    /// releaseTracker() drops m_tracker while the consumer and the ices bound
    /// to that thread live on. Written once in the constructor.
    nx::network::aio::AbstractAioThread* m_aioThread = nullptr;
    std::shared_ptr<Dtls> m_dtls;
    std::vector<IceCandidate> m_iceCandidates;

    // Transcoding output stream.
    nx::core::transcoding::Settings m_transcodingSettings;
    std::optional<nx::vms::api::ResolutionData> m_resolution;
    std::optional<nx::vms::api::ResolutionData> m_resolutionWhenTranscoding;

    bool m_stopped = false;
    /// Set by stopUnsafe(). ~Session skips the relay fallback timer's pleaseStopSync() when it
    /// is set: the timer is already stopped, and after the quiesce the destructor may run on a
    /// foreign AIO thread, where pleaseStopSync() would assert. The write is published to the
    /// destroying thread by the shared_ptr reference-count release it precedes.
    bool m_quiesced = false;
    createDataProviderFactory m_providerFactory;
    WarmUpProvider m_warmUpProvider;
    int m_version = 0;

    std::vector<std::unique_ptr<IceUdp>> m_udpIces;
    /// The ice that has locked this session, null while unlocked. May point at a pool-owned
    /// IceTcp, which outlives the session - never dereferenced beyond detachActiveIce(), and
    /// every ice clears it in stopUnsafe() before it can die. Touched only on aioThread().
    Ice* m_activeIce = nullptr;
    /** What connectedPath() reports; guarded by m_mutex. */
    std::optional<ConnectedPath> m_connectedPath;
    std::unique_ptr<IceUdp> m_punchedUdpIce;
    std::unique_ptr<IceRelay> m_relayIce;
    nx::network::aio::Timer m_relayFallbackTimer;
    bool m_relayArmed = false; ///< Fallback timer has been started.
    bool m_relayStarted = false; ///< startRelay has been (or won't be) called.
    bool m_directProbeSucceeded =
        false; ///< Punch response seen since the last (re)arm. Guarded by m_mutex.
    int m_relayFallbackExtensionCount = 0; ///< Guarded by m_mutex.
    std::unique_ptr<Srflx> m_srflx;
    std::unique_ptr<IceCandidate> m_remoteSrflx;

    // Split-horizon state (guarded by m_mutex). See Session::start().
    QList<nx::network::SocketAddress> m_deferredHostAddresses;
    std::optional<nx::network::HostAddress> m_reflexiveIp; ///< Our public IP, from srflx.
    std::vector<nx::network::HostAddress> m_peerReflexiveIps;
    std::vector<nx::network::SocketAddress> m_stunServers;
    SessionPool* m_sessionPool;
    SessionConfig m_config;

    // Reap clock for SessionPool's garbage collector. While idle nothing
    // refreshes it, so it ages out and the session is idle-reaped. While
    // streaming, onInboundActivity() restarts it on every inbound packet, so it
    // ages out only when the peer goes silent (RTCP/consent/relay data all
    // stop) - reaping a stream whose peer has vanished, which the direct path's
    // consent checks catch but the relay path does not.
    nx::utils::ElapsedTimer m_timer{nx::utils::ElapsedTimerState::started};

    // Lock (streaming) state. Explicit rather than encoded as !m_timer.isValid()
    // so m_timer can double as the streaming liveness clock above.
    bool m_locked = false;
    bool isLocked() const { return m_locked; }
};

} // namespace nx::webrtc
