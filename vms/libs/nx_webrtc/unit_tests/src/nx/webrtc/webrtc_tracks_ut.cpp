// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include <gtest/gtest.h>

#include <nx/network/socket_global.h>
#include <nx/network/ssl/certificate.h>
#include <nx/network/ssl/context.h>
#include <nx/vms/common/system_context.h>
#include <nx/webrtc/webrtc_session.h>
#include <nx/webrtc/webrtc_tracks.h>

namespace nx::webrtc::test {

/**
 * Covers the m-line order a camera switch pairs departing devices in: the peer binds its tiles
 * to the freed m-lines from the lowest mid up, so the server must hand them out in that order
 * whatever the device ids sort to.
 */
class WebRtcTracks: public ::testing::Test
{
protected:
    virtual void SetUp() override
    {
        // Tracks read the purpose off their session, and a Session needs the network subsystem
        // and a default certificate to be constructed at all.
        m_socketGlobals = std::make_unique<nx::network::SocketGlobalsHolder>();
        const auto pem =
            nx::network::ssl::makeCertificateAndKey({"Common Name", "US", "Organization"});
        ASSERT_TRUE(nx::network::ssl::Context::instance()->setDefaultCertificate(pem));
        m_systemContext = std::make_unique<nx::vms::common::SystemContext>(
            /*peerId*/ nx::Uuid::createUuid());
        m_session = std::make_unique<Session>(
            m_systemContext.get(),
            /*sessionPool*/ nullptr,
            [](nx::Uuid /*deviceId*/,
               std::optional<std::chrono::milliseconds> /*positionMs*/,
               nx::vms::api::StreamIndex /*stream*/,
               std::optional<float> /*speedOpt*/,
               std::string_view /*sessionId*/)
            {
                return AbstractCameraDataProviderPtr();
            },
            Session::generateLocalUfrag(),
            nx::network::SocketAddress(),
            SessionConfig(),
            /*stunServers*/ std::vector<nx::network::SocketAddress>(),
            Purpose::send);
        m_tracks = std::make_unique<TracksForSend>(m_session.get());
    }

    virtual void TearDown() override
    {
        m_tracks.reset();
        m_session.reset(); //< SystemContextAware must go before the context it points at.
        m_systemContext.reset();
        m_socketGlobals.reset();
    }

    void addVideoTrack(nx::Uuid deviceId)
    {
        auto track = std::make_unique<Track>();
        track->deviceId = deviceId;
        track->trackType = TrackType::video;
        track->offerState = TrackState::active; //< addTrack reclaims a parked track's m-line.
        m_tracks->addTrack(std::move(track));
    }

    std::unique_ptr<TracksForSend> m_tracks;

private:
    std::unique_ptr<nx::network::SocketGlobalsHolder> m_socketGlobals;
    std::unique_ptr<nx::vms::common::SystemContext> m_systemContext;
    std::unique_ptr<Session> m_session;
};

TEST_F(WebRtcTracks, orderByMidFollowsTheMlineOrderNotTheDeviceIdOrder)
{
    // Device ids that sort the other way round from their m-lines.
    const nx::Uuid first("{cccccccc-0000-0000-0000-000000000000}");
    const nx::Uuid second("{bbbbbbbb-0000-0000-0000-000000000000}");
    const nx::Uuid third("{aaaaaaaa-0000-0000-0000-000000000000}");
    addVideoTrack(first);
    addVideoTrack(second);
    addVideoTrack(third);
    ASSERT_LT(m_tracks->videoTrack(first)->mid, m_tracks->videoTrack(second)->mid);
    ASSERT_LT(m_tracks->videoTrack(second)->mid, m_tracks->videoTrack(third)->mid);

    const std::vector<nx::Uuid> byMid{first, second, third};
    EXPECT_EQ(m_tracks->orderByMid({third, second, first}), byMid);
    EXPECT_EQ(m_tracks->orderByMid({second, third, first}), byMid);
}

TEST_F(WebRtcTracks, orderByMidKeepsADeviceWithoutAVideoTrackLast)
{
    const nx::Uuid withTrack("{cccccccc-0000-0000-0000-000000000000}");
    const nx::Uuid withoutTrack("{aaaaaaaa-0000-0000-0000-000000000000}");
    addVideoTrack(withTrack);

    const std::vector<nx::Uuid> expected{withTrack, withoutTrack};
    EXPECT_EQ(m_tracks->orderByMid({withoutTrack, withTrack}), expected);
}

} // namespace nx::webrtc::test
