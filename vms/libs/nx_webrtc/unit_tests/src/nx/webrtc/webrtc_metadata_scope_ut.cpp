// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include <future>

#include <gtest/gtest.h>

#include <nx/network/aio/basic_pollable.h>
#include <nx/network/socket_global.h>
#include <nx/network/ssl/certificate.h>
#include <nx/network/ssl/context.h>
#include <nx/vms/common/system_context.h>
#include <nx/webrtc/webrtc_session.h>

namespace nx::webrtc::test {

/**
 * Covers the per-device overlay-metadata subscription contract of the
 * {"metadata": bool[, "deviceId": uuid]} data-channel command (multi-camera
 * layout sessions): a scoped command affects only the named device, devices
 * without an override follow the session default, and the session-wide form
 * resets every override. Exercised through a real Session (Purpose::send) via
 * Streamer::onDataChannelString, with no providers - the provider-side flag is
 * plain forwarding and is covered by functional tests.
 */
class WebRtcMetadataScope: public ::testing::Test
{
protected:
    virtual void SetUp() override
    {
        // The session binds its consumer and tracker to an AIO thread, so the
        // network subsystem must exist (this binary runs plain gtest_main,
        // which does not initialize it).
        m_socketGlobals = std::make_unique<nx::network::SocketGlobalsHolder>();

        // Session construction reads the process-wide default server
        // certificate; a unit test process has none, so install a throwaway one.
        const auto pem =
            nx::network::ssl::makeCertificateAndKey({"Common Name", "US", "Organization"});
        ASSERT_TRUE(nx::network::ssl::Context::instance()->setDefaultCertificate(pem));

        // Session is SystemContextAware, which requires a context; it reaches
        // for metrics() through it. A bare context with its own peer id is all
        // this test needs.
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

        m_pollable = std::make_unique<nx::network::aio::BasicPollable>(m_session->aioThread());
    }

    virtual void TearDown() override
    {
        if (m_pollable)
        {
            m_pollable->pleaseStopSync();
            m_pollable.reset();
        }
        m_session.reset(); //< SystemContextAware must go before the context it points at.
        m_systemContext.reset();
        m_socketGlobals.reset();
    }

    void command(const std::string& json)
    {
        m_session->transceiver()->onDataChannelString(json, /*streamId*/ 0);
    }

    /**
     * Reads the consumer state on the session's muxing thread. Posted calls on
     * one AIO thread run in post order, so the read observes every mutation the
     * test issued before it.
     */
    bool metadataEnabledFor(nx::Uuid deviceId)
    {
        return readInSessionThread(
            [this, deviceId]()
            {
                return m_session->consumer()->isMetadataEnabledFor(deviceId);
            });
    }

    bool sessionDefault()
    {
        return readInSessionThread(
            [this]()
            {
                return m_session->consumer()->isMetadataEnabled();
            });
    }

private:
    bool readInSessionThread(std::function<bool()> read)
    {
        std::promise<bool> promise;
        m_pollable->dispatch(
            [&promise, read = std::move(read)]()
            {
                promise.set_value(read());
            });
        return promise.get_future().get();
    }

private:
    std::unique_ptr<nx::network::SocketGlobalsHolder> m_socketGlobals;
    std::unique_ptr<nx::vms::common::SystemContext> m_systemContext;
    std::unique_ptr<Session> m_session;
    std::unique_ptr<nx::network::aio::BasicPollable> m_pollable;
};

TEST_F(WebRtcMetadataScope, scopedSubscriptionAffectsOnlyTheNamedDevice)
{
    const auto camA = nx::Uuid::createUuid();
    const auto camB = nx::Uuid::createUuid();

    EXPECT_FALSE(sessionDefault());
    EXPECT_FALSE(metadataEnabledFor(camA));

    command("{\"metadata\": true, \"deviceId\": \"" + camA.toSimpleStdString() + "\"}");
    EXPECT_TRUE(metadataEnabledFor(camA));
    EXPECT_FALSE(metadataEnabledFor(camB));
    EXPECT_FALSE(sessionDefault()); //< A scoped subscribe must not flip the default.
}

TEST_F(WebRtcMetadataScope, deviceWithoutOverrideFollowsSessionDefault)
{
    const auto camA = nx::Uuid::createUuid();
    const auto camB = nx::Uuid::createUuid();

    command("{\"metadata\": true}");
    EXPECT_TRUE(sessionDefault());
    EXPECT_TRUE(metadataEnabledFor(camB));

    command("{\"metadata\": false, \"deviceId\": \"" + camA.toSimpleStdString() + "\"}");
    EXPECT_FALSE(metadataEnabledFor(camA));
    EXPECT_TRUE(metadataEnabledFor(camB));
}

TEST_F(WebRtcMetadataScope, sessionWideCommandResetsEveryOverride)
{
    const auto camA = nx::Uuid::createUuid();

    command("{\"metadata\": true, \"deviceId\": \"" + camA.toSimpleStdString() + "\"}");
    EXPECT_TRUE(metadataEnabledFor(camA));

    command("{\"metadata\": false}");
    EXPECT_FALSE(metadataEnabledFor(camA)); //< The override is gone, not shadowed.

    command("{\"metadata\": true}");
    EXPECT_TRUE(metadataEnabledFor(camA)); //< Follows the default again.
}

TEST_F(WebRtcMetadataScope, legacyIntegerFormKeepsWorking)
{
    const auto camA = nx::Uuid::createUuid();

    command("{\"metadata\": 1}");
    EXPECT_TRUE(sessionDefault());
    EXPECT_TRUE(metadataEnabledFor(camA));

    command("{\"metadata\": 0}");
    EXPECT_FALSE(sessionDefault());
    EXPECT_FALSE(metadataEnabledFor(camA));
}

} // namespace nx::webrtc::test
