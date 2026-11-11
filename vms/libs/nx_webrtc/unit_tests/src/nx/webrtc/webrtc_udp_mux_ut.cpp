// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <nx/network/socket_global.h>
#include <nx/network/ssl/certificate.h>
#include <nx/network/ssl/context.h>
#include <nx/network/stun/message.h>
#include <nx/network/stun/message_parser.h>
#include <nx/network/stun/message_serializer.h>
#include <nx/network/system_socket.h>
#include <nx/utils/timer_manager.h>
#include <nx/vms/common/system_context.h>
#include <nx/webrtc/webrtc_dtls.h>
#include <nx/webrtc/webrtc_ice.h>
#include <nx/webrtc/webrtc_session.h>
#include <nx/webrtc/webrtc_session_pool.h>
#include <nx/webrtc/webrtc_udp_mux.h>
#include <nx/webrtc/webrtc_utils.h>

namespace nx::webrtc::test {

namespace stun = nx::network::stun;

namespace {

constexpr char kSessionId[] = "sess";
constexpr char kSessionPwd[] = "0123456789abcdef01234567";

/** A check of the peer for our session kSessionId, signed with pwd. */
nx::Buffer peerCheck(const std::string& pwd)
{
    // The peer's USERNAME is "<our ufrag>:<its ufrag>", and it signs with our ice-pwd: the same
    // shape that buildControlledBindingRequest() makes toward a peer.
    return buildControlledBindingRequest(
        /*remoteUfrag*/ kSessionId,
        /*localUfrag*/ "peer",
        /*remotePwd*/ pwd,
        /*priority*/ 1,
        /*tieBreaker*/ 42);
}

std::optional<std::string> sessionPwd(const std::string& sessionId)
{
    if (sessionId == kSessionId)
        return kSessionPwd;
    return std::nullopt;
}

} // namespace

TEST(WebRtcUdpMux, CheckSignedWithTheSessionPwdNamesTheSession)
{
    EXPECT_EQ(
        authenticatedSessionId(peerCheck(kSessionPwd), sessionPwd),
        std::optional<std::string>(kSessionId));
}

// A source that knows only the short ufrag must not claim a table entry or a session's path.
TEST(WebRtcUdpMux, CheckWithAWrongPwdNamesNoSession)
{
    EXPECT_EQ(
        authenticatedSessionId(peerCheck("wrong-password-0123456789"), sessionPwd),
        std::nullopt);
}

TEST(WebRtcUdpMux, CheckForAnUnknownSessionNamesNoSession)
{
    EXPECT_EQ(
        authenticatedSessionId(
            peerCheck(kSessionPwd),
            [](const std::string&)
            {
                return std::nullopt;
            }),
        std::nullopt);
}

TEST(WebRtcUdpMux, OnlyBindingRequestsIntroducePeers)
{
    EXPECT_EQ(authenticatedSessionId(nx::Buffer("not a stun message"), sessionPwd), std::nullopt);

    stun::Message response;
    response.header.messageClass = stun::MessageClass::successResponse;
    response.header.method = stun::MethodType::bindingMethod;
    response.newAttribute<stun::attrs::UserName>("sess:peer");
    stun::MessageSerializer serializer;
    serializer.setAlwaysAddFingerprint(true);
    EXPECT_EQ(authenticatedSessionId(serializer.serialized(response), sessionPwd), std::nullopt);
}

//-------------------------------------------------------------------------------------------------

namespace {

/** Long enough for a loaded machine; a missing answer costs this much once. */
constexpr unsigned int kAnswerTimeoutMs = 3000;

/** How long a test waits for an answer that must not come. */
constexpr unsigned int kNoAnswerTimeoutMs = 300;

/** The ICE ufrag of the browser in the SDP answer of a test. */
constexpr char kPeerUfrag[] = "peer";

/** Longer than the silence after which the mux moves a session's path to a new address. */
constexpr auto kPathSilence = std::chrono::milliseconds(3500);

/** At least 1, so that a socket timeout of 0 does not mean "no timeout". */
unsigned int remainingMs(std::chrono::steady_clock::time_point deadline)
{
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    return (unsigned int) std::max<int64_t>(left.count(), 1);
}

/** RFC 7983: the first byte of a DTLS record is 20 to 63. */
bool isDtlsRecord(const nx::Buffer& datagram)
{
    return !datagram.empty() && (uint8_t) datagram[0] >= 20 && (uint8_t) datagram[0] <= 63;
}

/** The browser's end of a session on the shared port: a UDP socket on localhost. */
class Peer
{
public:
    explicit Peer(uint16_t sharedPort):
        m_sharedPort(sharedPort)
    {
        EXPECT_TRUE(
            m_socket.bind(nx::network::SocketAddress(nx::network::HostAddress::localhost, 0)));
    }

    /**
     * Sends a binding request for the session, signed with pwd. A browser that has no answer of
     * its own yet sends an empty localUfrag: USERNAME "<session ufrag>:".
     * @return The transaction id of the request.
     */
    nx::Buffer sendCheck(
        const std::string& sessionId,
        const std::string& pwd,
        const std::string& localUfrag = "")
    {
        const nx::Buffer request = buildControlledBindingRequest(
            sessionId,
            localUfrag,
            pwd,
            /*priority*/ 1,
            /*tieBreaker*/ 42);
        send(request);

        stun::Message message;
        stun::MessageParser parser;
        parser.setMessage(&message);
        size_t parsed = 0;
        parser.parse(request, &parsed);
        return message.header.transactionId;
    }

    void send(const nx::Buffer& datagram)
    {
        EXPECT_TRUE(m_socket.sendTo(
            datagram.data(),
            datagram.size(),
            nx::network::SocketAddress(nx::network::HostAddress::localhost, m_sharedPort)));
    }

    /** A datagram from the shared port, or null when none came within timeoutMs. */
    std::optional<nx::Buffer> receiveDatagram(unsigned int timeoutMs = kAnswerTimeoutMs)
    {
        EXPECT_TRUE(m_socket.setRecvTimeout(timeoutMs));
        std::array<char, 2048> buffer;
        nx::network::SocketAddress source;
        const int size = m_socket.recvFrom(buffer.data(), buffer.size(), &source);
        if (size <= 0)
            return std::nullopt;
        EXPECT_EQ(source.port, m_sharedPort);
        return nx::Buffer(buffer.data(), size);
    }

    /**
     * The next STUN answer from the shared port, or null when none came within timeoutMs. Skips
     * the DTLS records of the session.
     */
    std::optional<stun::Message> receive(unsigned int timeoutMs = kAnswerTimeoutMs)
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (const auto datagram = receiveDatagram(remainingMs(deadline)))
        {
            if (isDtlsRecord(*datagram))
                continue;

            stun::Message message;
            stun::MessageParser parser;
            parser.setMessage(&message);
            size_t parsed = 0;
            if (parser.parse(*datagram, &parsed) != nx::network::server::ParserState::done)
                return std::nullopt;
            return message;
        }
        return std::nullopt;
    }

    void close() { m_socket.close(); }

private:
    const uint16_t m_sharedPort;
    nx::network::UDPSocket m_socket{AF_INET};
};

/** The browser's DTLS client of a session, over the datagrams of a Peer. */
class DtlsClient: public AbstractDtlsDelegate
{
public:
    DtlsClient()
    {
        EXPECT_TRUE(
            m_pem.parse(nx::network::ssl::makeCertificateAndKey({"Peer", "US", "Organization"})));
        m_dtls = std::make_unique<Dtls>(kPeerUfrag, m_pem, Dtls::Type::client);
    }

    /** The SDP answer of the browser: the data channel, its ICE credentials and certificate. */
    std::string answerSdp() const
    {
        constexpr char kHexDigits[] = "0123456789ABCDEF";
        const auto hash = nx::network::ssl::CertificateView(m_pem.certificate().x509()).sha256();
        std::string fingerprint;
        for (const auto c: hash)
        {
            const auto byte = (uint8_t) c;
            if (!fingerprint.empty())
                fingerprint += ':';
            fingerprint += kHexDigits[byte >> 4];
            fingerprint += kHexDigits[byte & 0xf];
        }
        return "v=0\n"
               "a=group:BUNDLE "
            + std::to_string(kDataChannelMid)
            + "\n"
              "a=ice-ufrag:"
            + kPeerUfrag
            + "\n"
              "a=ice-pwd:0123456789abcdef0123456789\n"
              "a=fingerprint:sha-256 "
            + fingerprint
            + "\n"
              "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\n"
              "a=mid:"
            + std::to_string(kDataChannelMid) + "\n";
    }

    void setServerFingerprint(const Fingerprint& fingerprint)
    {
        m_dtls->setFingerprint(fingerprint);
    }

    /** Runs the handshake through peer. True once both sides have finished it. */
    bool handshake(Peer* peer)
    {
        Dtls::Status status = m_dtls->handlePacket(nullptr, 0, this); //< Makes the ClientHello.
        sendOutput(peer);
        while (status != Dtls::Status::streaming && status != Dtls::Status::error)
        {
            const auto datagram = peer->receiveDatagram();
            if (!datagram)
                return false;
            if (!isDtlsRecord(*datagram))
                continue;
            status = m_dtls->handlePacket(
                (const uint8_t*) datagram->data(),
                (int) datagram->size(),
                this);
            sendOutput(peer);
        }
        return status == Dtls::Status::streaming;
    }

    /** Sends data to the session over the DTLS of the handshake, from peer. */
    bool send(Peer* peer, const nx::Buffer& data)
    {
        if (m_dtls->writePacket((const uint8_t*) data.data(), (int) data.size(), this)
            == Dtls::Status::error)
        {
            return false;
        }
        sendOutput(peer);
        return true;
    }

    virtual void writeDtlsPacket(const char* data, int size) override
    {
        m_output.emplace_back(data, size);
    }

    /**
     * Whether data of the session, encrypted with the keys of the handshake, came to peer within
     * kAnswerTimeoutMs.
     */
    bool receive(Peer* peer)
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kAnswerTimeoutMs);
        m_dataReceived = false;
        while (const auto datagram = peer->receiveDatagram(remainingMs(deadline)))
        {
            if (!isDtlsRecord(*datagram))
                continue;
            m_dtls->handlePacket((const uint8_t*) datagram->data(), (int) datagram->size(), this);
            if (m_dataReceived)
                return true;
        }
        return false;
    }

    virtual bool onDtlsData(const uint8_t* /*data*/, int /*size*/) override
    {
        m_dataReceived = true;
        return true;
    }

private:
    void sendOutput(Peer* peer)
    {
        for (const auto& datagram: m_output)
            peer->send(datagram);
        m_output.clear();
    }

private:
    nx::network::ssl::Pem m_pem;
    std::unique_ptr<Dtls> m_dtls;
    std::vector<nx::Buffer> m_output;
    bool m_dataReceived = false;
};

/** The sha-256 fingerprint of the SDP offer of a session. */
Fingerprint offerFingerprint(const std::string& offer)
{
    static const std::string kPrefix = "a=fingerprint:sha-256 ";
    Fingerprint result;
    const size_t start = offer.find(kPrefix);
    if (!NX_ASSERT(start != std::string::npos))
        return result;

    result.type = Fingerprint::Type::sha256;
    for (size_t i = start + kPrefix.size(); i + 1 < offer.size() && std::isxdigit(offer[i]);
         i += 3)
        result.hash.push_back((uint8_t) std::stoi(offer.substr(i, 2), nullptr, 16));
    return result;
}

uint32_t crc32c(const uint8_t* data, size_t size)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0x82F63B78 & (0 - (crc & 1)));
    }
    return ~crc;
}

/** An SCTP INIT. The data channel of a session answers it at once, with an INIT ACK. */
nx::Buffer sctpInit()
{
    std::array<uint8_t, 32> packet{0x13, 0x88, 0x13, 0x88, //< Source and destination port 5000.
                                   0,    0,    0,    0, //< Verification tag, 0 in an INIT.
                                   0,    0,    0,    0, //< Checksum, filled below.
                                   1,    0,    0,    20, //< Chunk INIT of 20 bytes.
                                   0x12, 0x34, 0x56, 0x78, //< Initiate tag.
                                   0,    1,    0,    0, //< Receiver window.
                                   0x04, 0x00, 0x04, 0x00, //< Outbound and inbound streams.
                                   0,    0,    0,    1}; //< Initial TSN.
    const uint32_t checksum = crc32c(packet.data(), packet.size());
    for (int i = 0; i < 4; ++i)
        packet[8 + i] = (uint8_t) (checksum >> (8 * i)); //< SCTP sends the CRC32c low byte first.
    return nx::Buffer((const char*) packet.data(), packet.size());
}

} // namespace

/**
 * A real SessionPool serves the shared UDP port on localhost, without UPnP. The test plays the
 * browser: it sends the binding requests a browser sends and reads the server's answers, so the
 * mux runs its whole path from the socket to the session's ice and back.
 */
class WebRtcSharedUdpPort: public ::testing::Test
{
protected:
    virtual void SetUp() override
    {
        m_socketGlobals = std::make_unique<nx::network::SocketGlobalsHolder>();
        const auto pem =
            nx::network::ssl::makeCertificateAndKey({"Common Name", "US", "Organization"});
        ASSERT_TRUE(nx::network::ssl::Context::instance()->setDefaultCertificate(pem));
        m_systemContext = std::make_unique<nx::vms::common::SystemContext>(
            /*peerId*/ nx::Uuid::createUuid());
        m_timerManager = std::make_unique<nx::utils::TimerManager>("webrtc_udp_mux_ut");
        m_pool = std::make_unique<SessionPool>(
            /*realm*/ "occurra",
            /*cloudHost*/ "dev.occurra.com",
            m_systemContext.get(),
            m_timerManager.get(),
            QnFfmpegVideoTranscoder::TranscodePolicy{},
            /*defaultStunServers*/ std::vector<nx::network::SocketAddress>{});
        ASSERT_TRUE(m_pool->startSharedUdpPort(/*port*/ 0, /*upnpEnabled*/ false, "test"));
        ASSERT_NE(m_pool->sharedUdpPort(), 0);
    }

    virtual void TearDown() override
    {
        m_sessions.clear(); //< The pool stops them, and the ices of the shared port first.
        m_pool.reset();
        m_timerManager.reset();
        m_systemContext.reset();
        m_socketGlobals.reset();
    }

    std::shared_ptr<Session> createSession()
    {
        auto session = m_pool->create(
            /*providerFactory*/ nullptr,
            Purpose::data,
            nx::network::SocketAddress(nx::network::HostAddress::localhost, 7010),
            SessionConfig{},
            {nx::network::SocketAddress(nx::network::HostAddress::localhost, 0)});
        EXPECT_TRUE(session);
        m_sessions.push_back(session);
        return session;
    }

    std::unique_ptr<Peer> makePeer() { return std::make_unique<Peer>(m_pool->sharedUdpPort()); }

private:
    std::unique_ptr<nx::network::SocketGlobalsHolder> m_socketGlobals;
    std::unique_ptr<nx::vms::common::SystemContext> m_systemContext;
    std::unique_ptr<nx::utils::TimerManager> m_timerManager;
    std::vector<std::shared_ptr<Session>> m_sessions;

protected:
    std::unique_ptr<SessionPool> m_pool;
};

// A new source address with a check signed by the session's password gets the session's answer
// from the shared port.
TEST_F(WebRtcSharedUdpPort, AnswersAnAuthenticatedCheckFromANewAddress)
{
    const auto session = createSession();
    auto peer = makePeer();

    const nx::Buffer transactionId = peer->sendCheck(session->id(), session->localPwd());
    const auto answer = peer->receive();
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->header.messageClass, stun::MessageClass::successResponse);
    EXPECT_EQ(answer->header.transactionId, transactionId);
}

// The second check from the same address takes the table path to the same ice.
TEST_F(WebRtcSharedUdpPort, AnswersRepeatedChecksFromTheSameAddress)
{
    const auto session = createSession();
    auto peer = makePeer();

    for (int i = 0; i < 3; ++i)
    {
        const nx::Buffer transactionId = peer->sendCheck(session->id(), session->localPwd());
        const auto answer = peer->receive();
        ASSERT_TRUE(answer) << "check " << i;
        EXPECT_EQ(answer->header.transactionId, transactionId) << "check " << i;
    }
}

// Two sessions share the port; each peer reaches its own session by the ufrag it names.
TEST_F(WebRtcSharedUdpPort, RoutesEachPeerToItsSession)
{
    const auto first = createSession();
    const auto second = createSession();
    auto firstPeer = makePeer();
    auto secondPeer = makePeer();

    const nx::Buffer firstId = firstPeer->sendCheck(first->id(), first->localPwd());
    const nx::Buffer secondId = secondPeer->sendCheck(second->id(), second->localPwd());

    const auto firstAnswer = firstPeer->receive();
    const auto secondAnswer = secondPeer->receive();
    ASSERT_TRUE(firstAnswer);
    ASSERT_TRUE(secondAnswer);
    EXPECT_EQ(firstAnswer->header.messageClass, stun::MessageClass::successResponse);
    EXPECT_EQ(secondAnswer->header.messageClass, stun::MessageClass::successResponse);
    EXPECT_EQ(firstAnswer->header.transactionId, firstId);
    EXPECT_EQ(secondAnswer->header.transactionId, secondId);
}

// A check signed with another password gets nothing: the mux drops it before any ice exists.
TEST_F(WebRtcSharedUdpPort, IgnoresACheckWithAWrongPassword)
{
    const auto session = createSession();
    auto peer = makePeer();

    peer->sendCheck(session->id(), "wrong-password-0123456789");
    EXPECT_FALSE(peer->receive(kNoAnswerTimeoutMs));

    // The same address still gets in with the right password.
    const nx::Buffer transactionId = peer->sendCheck(session->id(), session->localPwd());
    const auto answer = peer->receive();
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->header.transactionId, transactionId);
}

// A peer that goes away leaves the port serving the others: on Windows the answer to its closed
// socket comes back as an ICMP "port unreachable", reported as a read error of the shared socket.
TEST_F(WebRtcSharedUdpPort, KeepsServingAfterAPeerGoesAway)
{
    const auto session = createSession();
    auto gone = makePeer();
    gone->sendCheck(session->id(), session->localPwd());
    gone->close();

    const auto other = createSession();
    auto peer = makePeer();
    const nx::Buffer transactionId = peer->sendCheck(other->id(), other->localPwd());
    const auto answer = peer->receive();
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->header.transactionId, transactionId);
}

// A peer whose address changes, as when its NAT rebinds, keeps its session: once the session's
// path is silent, an authenticated check from the new address takes the path over, and the
// session's traffic follows. A new ice in its place would get no DTLS record through.
TEST_F(WebRtcSharedUdpPort, MovesASilentPathToTheNewAddressOfItsPeer)
{
    const auto session = createSession();
    DtlsClient dtls;
    dtls.setServerFingerprint(offerFingerprint(session->constructSdp()));
    session->processSdpAnswer(dtls.answerSdp());

    auto before = makePeer();
    before->sendCheck(session->id(), session->localPwd(), kPeerUfrag);
    ASSERT_TRUE(before->receive());
    ASSERT_TRUE(dtls.handshake(before.get()));

    std::this_thread::sleep_for(kPathSilence);

    auto after = makePeer();
    const nx::Buffer transactionId =
        after->sendCheck(session->id(), session->localPwd(), kPeerUfrag);
    const auto answer = after->receive();
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->header.transactionId, transactionId);

    ASSERT_TRUE(dtls.send(after.get(), sctpInit()));
    EXPECT_TRUE(dtls.receive(after.get()));
}

} // namespace nx::webrtc::test
