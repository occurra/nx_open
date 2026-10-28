// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nx/network/socket_common.h>
#include <nx/network/ssl/certificate.h>

namespace nx::webrtc {

constexpr uint16_t kDefaultSctpPort = 5000;
constexpr int kSctpMaxMessageSize = 65536;

/**
 * Advertised max-message-size for the data channel, every session alike. A
 * spec-conforming peer (the toolbox's libdatachannel) caps its sends at what
 * we advertise; our receive path reassembles SCTP fragments regardless. The
 * largest single message is one video frame (segments travel frame by frame;
 * a 4K key frame runs single-digit megabytes) or a metadata batch with
 * embedded images, so 16 MB leaves ample headroom.
 */
constexpr int kSctpMaxMessageSizeData = 16 * 1024 * 1024;

using namespace nx::network;
using namespace nx;

enum class Purpose
{
    send,
    recv,
    sendrecv,

    /// Datachannel-only session: no media tracks, no consumer/providers. The
    /// SDP carries a single application m-line and every byte rides the SCTP
    /// datachannel (see DataPeer). This end stays the offerer, exactly as for
    /// browser peers; the answering end (the AI agent toolbox) runs
    /// libdatachannel and behaves browser-identically.
    data,
};

class Dtls;

class Session;
class SessionPool;

using SessionPtr = std::shared_ptr<Session>;

struct SessionDescription
{
    std::string id;
    std::string remoteUfrag;
    std::string localPwd;
    std::string remotePwd; ///< Peer's ice-pwd; keys MESSAGE-INTEGRITY of checks we send.
    std::uint64_t tieBreaker = 0; ///< This agent's ICE tie-breaker (role-conflict resolution).
    std::shared_ptr<Dtls> dtls;
};

struct Fingerprint
{
    std::vector<uint8_t> hash; //< In binary format.
    NX_REFLECTION_ENUM_IN_CLASS(Type, sha1, sha224, sha256, sha384, sha512, md5, md2, none)
    Type type = Type::none;
    static Fingerprint parse(std::string_view sv);
};

enum TrackState
{
    inactive,
    active
};

struct SdpParseResult
{
    Fingerprint fingerprint;
    std::string iceUfrag;
    std::string icePwd;
    std::map<int, TrackState> tracksState;

    static SdpParseResult parse(const std::string& sdp);
};

struct NX_WEBRTC_API IceCandidate
{
    static constexpr int kIceMaxPriority = std::numeric_limits<int>::max();
    static constexpr int kIceMinIndex = 0;

    // Debug purpose.
    enum Filter : int
    {
        None = 0,
        UdpHost = 1,
        TcpHost = 2,
        UdpSrflx = 4,
        UdpRelay = 8,
        All = UdpHost | TcpHost | UdpSrflx | UdpRelay,
    };
    enum Protocol
    {
        UDP,
        TCP
    };
    enum Type
    {
        Host,
        Srflx,
        Prflx, //< Not sure if this type can exist in SDP.
        Relay
    };
    enum TcpType
    {
        Active,
        Passive,
        Actpass
    };

    unsigned int foundation = 0; //< Something like an ID.
    int componentId = 1; //< 1 for RTP, 2 for RTCP. Always 1 for rtcp-mux
    Protocol protocol = Protocol::UDP;
    int priority = 0;
    nx::network::SocketAddress address;
    Type type = Type::Host;
    std::optional<TcpType> tcpType;
    std::optional<nx::network::SocketAddress> relatedAddress;

    static IceCandidate constructTcpHost(
        unsigned int _foundation,
        int _priority,
        const nx::network::SocketAddress& _address);
    static IceCandidate constructUdpHost(
        unsigned int _foundation,
        int _priority,
        const nx::network::SocketAddress& _address);
    static IceCandidate constructUdpSrflx(
        unsigned int _foundation,
        int _priority,
        const nx::network::SocketAddress& _address,
        const nx::network::SocketAddress& _relatedAddress);

    static IceCandidate parse(const std::string& candidate);
    static Filter parseFilter(const std::string& filter);
    std::string serialize() const;
    bool matchFilter(const Filter filter) const;
};

/** The name of the path an ice of type carries, as the log and the health counters show it. */
NX_WEBRTC_API std::string pathName(IceCandidate::Filter type);

struct TurnEndpoint
{
    nx::network::SocketAddress address;
    nx::network::TransportProtocol transport =
        nx::network::TransportProtocol::udp; //< RFC 7065 default for the "turn:" scheme.
};

struct TurnServerInfo
{
    std::vector<TurnEndpoint> endpoints; //< Cloud order, used as the failover preference order.
    std::string user;
    std::string password;
    std::string realm;
    bool longTermAuth = false;

    bool isNull() const { return endpoints.empty(); } //< Replaces the old address.isNull() checks.
};

struct OauthInfo
{
    std::string kid;
    int lifetime;
    std::chrono::seconds expires_at;
    std::string sessionKey;
    std::string accessToken;
};

struct SessionConfig
{
    bool unreliableTransport = false;

    /// Advertise and emit the abs-capture-time RTP header extension
    /// (http://www.webrtc.org/experiments/rtp-hdrext/abs-capture-time).
    /// Disabled by default: the player gets RTP→UTC mappings via
    /// data-channel timestamp messages, which are explicit and stay
    /// consistent with the pre-roll PTS compression. abs-capture-time
    /// would carry the original capture UTC for pre-roll frames, which
    /// would conflict with the data-channel reports. Re-enable only
    /// when something downstream actually needs the extension (e.g.
    /// inter-stream sync via the browser's native capture-time path).
    bool addAbsCaptureTime = false;

    /// Advertise and emit the playout-delay RTP header extension with min=max=0
    /// (http://www.webrtc.org/experiments/rtp-hdrext/playout-delay). Puts Chrome
    /// into its low-latency render path so a decoded frame is shown immediately
    /// instead of being held by the playout buffer - important for archive seeks,
    /// where the paused single-shot stream otherwise stalled the compositor for
    /// hundreds of ms. Video only (see Transcoder::createRtpEncoder).
    bool addPlayoutDelay = true;
};

/**
 * Whether the address is unroutable from the public internet, so advertising
 * it to a remote peer can only produce a dead ICE candidate.
 *
 * Not HostAddress::isLocalNetwork(): that check misses CGNAT 100.64.0.0/10
 * (Tailscale v4) and matches only the fd00:/16 slice of the fc00::/7 ULA
 * range (Tailscale v6 is fd7a:...).
 */
NX_WEBRTC_API bool isPrivateScope(const nx::network::HostAddress& host);

/** Same-family binary IP equality; HostAddress::operator== is text-based. */
NX_WEBRTC_API bool sameIp(const nx::network::HostAddress& a, const nx::network::HostAddress& b);

/**
 * Public STUN servers both ends of a session use by default, so srflx behavior is identical on
 * both. Inline: the toolbox includes this header without linking the library.
 */
inline const std::vector<nx::network::SocketAddress>& defaultStunServers()
{
    static const std::vector<nx::network::SocketAddress> kStunServers = {
        nx::network::SocketAddress("stun.l.google.com:19302"),
        nx::network::SocketAddress("stun1.l.google.com:19302"),
        nx::network::SocketAddress("stun2.l.google.com:19302"),
    };
    return kStunServers;
}

/**
 * The STUN servers a session queries: the UDP endpoints of the TURN servers, then the defaults.
 * The TURN host answers STUN bindings too, a site that can relay can reach it, and both ends of
 * a session asking the same host get reflexive addresses the split horizon can compare.
 */
inline std::vector<nx::network::SocketAddress> stunServers(
    const TurnServerInfo& turnServer, const std::vector<nx::network::SocketAddress>& defaults)
{
    std::vector<nx::network::SocketAddress> result;
    for (const auto& endpoint: turnServer.endpoints)
    {
        if (endpoint.transport == nx::network::TransportProtocol::udp)
            result.push_back(endpoint.address);
    }
    result.insert(result.end(), defaults.begin(), defaults.end());
    return result;
}

} // namespace nx::webrtc
