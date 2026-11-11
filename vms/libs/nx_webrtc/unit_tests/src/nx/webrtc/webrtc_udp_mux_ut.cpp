// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include <array>
#include <memory>
#include <optional>
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
#include <nx/webrtc/webrtc_ice.h>
#include <nx/webrtc/webrtc_session.h>
#include <nx/webrtc/webrtc_session_pool.h>
#include <nx/webrtc/webrtc_udp_mux.h>

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
     * Sends a binding request for the session, signed with pwd, as a browser that has no answer
     * of its own yet does: USERNAME "<session ufrag>:".
     * @return The transaction id of the request.
     */
    nx::Buffer sendCheck(const std::string& sessionId, const std::string& pwd)
    {
        const nx::Buffer request = buildControlledBindingRequest(
            sessionId,
            /*localUfrag*/ "",
            pwd,
            /*priority*/ 1,
            /*tieBreaker*/ 42);
        EXPECT_TRUE(m_socket.sendTo(
            request.data(),
            request.size(),
            nx::network::SocketAddress(nx::network::HostAddress::localhost, m_sharedPort)));

        stun::Message message;
        stun::MessageParser parser;
        parser.setMessage(&message);
        size_t parsed = 0;
        parser.parse(request, &parsed);
        return message.header.transactionId;
    }

    /** The answer from the shared port, or null when none came within timeoutMs. */
    std::optional<stun::Message> receive(unsigned int timeoutMs = kAnswerTimeoutMs)
    {
        EXPECT_TRUE(m_socket.setRecvTimeout(timeoutMs));
        std::array<char, 2048> buffer;
        nx::network::SocketAddress source;
        const int size = m_socket.recvFrom(buffer.data(), buffer.size(), &source);
        if (size <= 0)
            return std::nullopt;
        EXPECT_EQ(source.port, m_sharedPort);

        stun::Message message;
        stun::MessageParser parser;
        parser.setMessage(&message);
        size_t parsed = 0;
        if (parser.parse(nx::Buffer(buffer.data(), size), &parsed)
            != nx::network::server::ParserState::done)
        {
            return std::nullopt;
        }
        return message;
    }

    void close() { m_socket.close(); }

private:
    const uint16_t m_sharedPort;
    nx::network::UDPSocket m_socket{AF_INET};
};

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

} // namespace nx::webrtc::test
