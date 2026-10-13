// Copyright 2026-present Occurra. All rights reserved.

#include <future>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QtCore/QByteArray>

#include <nx/network/aio/basic_pollable.h>
#include <nx/network/socket_global.h>
#include <nx/network/ssl/certificate.h>
#include <nx/network/ssl/context.h>
#include <nx/vms/common/system_context.h>
#include <nx/webrtc/webrtc_session.h>
#include <nx/webrtc/webrtc_streamer.h>

namespace nx::webrtc::test {

/**
 * Talk-back rides the video session's data channel (nx_webrtc Purpose::send),
 * the same one that carries the metadata subscription and playback commands.
 * The {"talk":{"deviceIds","on"}} control string (or its legacy single
 * "deviceId" form) and the deviceId-prefixed binary audio frames both reach
 * the TalkHandlers the media_server installs; a session without talk handlers
 * keeps treating a binary frame as a legacy JSON command, so nothing that
 * works today regresses.
 */
class WebRtcStreamerTalk: public ::testing::Test
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
        m_session.reset();
        m_systemContext.reset();
        m_socketGlobals.reset();
    }

    void installTalkHandlers()
    {
        TalkHandlers handlers;
        handlers.onControl = [this](const std::vector<nx::Uuid>& deviceIds, bool on)
        {
            m_controlCalls.push_back({deviceIds, on});
        };
        handlers.onAudio = [this](const nx::Uuid& deviceId, const std::string& opus)
        {
            m_audioCalls.push_back({deviceId, opus});
        };
        m_session->setTalkHandlers(std::move(handlers));
    }

    void sendString(const std::string& json)
    {
        m_session->transceiver()->onDataChannelString(json, /*streamId*/ 0);
    }

    void sendBinary(const std::string& data)
    {
        m_session->transceiver()->onDataChannelBinary(data, /*streamId*/ 0);
    }

    static std::string talkFrame(const nx::Uuid& deviceId, const std::string& opus)
    {
        const QByteArray id = deviceId.toRfc4122(); //< 16 raw bytes.
        std::string frame(id.constData(), (size_t) id.size());
        frame += opus;
        return frame;
    }

    bool metadataEnabledFor(nx::Uuid deviceId)
    {
        std::promise<bool> promise;
        m_pollable->dispatch(
            [this, deviceId, &promise]()
            {
                promise.set_value(m_session->consumer()->isMetadataEnabledFor(deviceId));
            });
        return promise.get_future().get();
    }

    struct ControlCall
    {
        std::vector<nx::Uuid> deviceIds;
        bool on;
    };
    struct AudioCall
    {
        nx::Uuid deviceId;
        std::string opus;
    };
    std::vector<ControlCall> m_controlCalls;
    std::vector<AudioCall> m_audioCalls;

private:
    std::unique_ptr<nx::network::SocketGlobalsHolder> m_socketGlobals;
    std::unique_ptr<nx::vms::common::SystemContext> m_systemContext;
    std::unique_ptr<Session> m_session;
    std::unique_ptr<nx::network::aio::BasicPollable> m_pollable;
};

TEST_F(WebRtcStreamerTalk, talkControlOnOffReachesHandler)
{
    installTalkHandlers();
    const auto cam = nx::Uuid::createUuid();

    sendString("{\"talk\":{\"deviceId\":\"" + cam.toSimpleStdString() + "\",\"on\":true}}");
    ASSERT_EQ(1u, m_controlCalls.size());
    EXPECT_EQ(std::vector<nx::Uuid>{cam}, m_controlCalls[0].deviceIds)
        << "a legacy single deviceId arrives as a one-element list";
    EXPECT_TRUE(m_controlCalls[0].on);

    sendString("{\"talk\":{\"deviceId\":\"" + cam.toSimpleStdString() + "\",\"on\":false}}");
    ASSERT_EQ(2u, m_controlCalls.size());
    EXPECT_EQ(std::vector<nx::Uuid>{cam}, m_controlCalls[1].deviceIds);
    EXPECT_FALSE(m_controlCalls[1].on);
}

TEST_F(WebRtcStreamerTalk, talkControlWithDeviceIdsArrayReachesHandlerWithTheWholeSet)
{
    installTalkHandlers();
    const auto camA = nx::Uuid::createUuid();
    const auto camB = nx::Uuid::createUuid();

    sendString(
        "{\"talk\":{\"deviceIds\":[\"" + camA.toSimpleStdString() + "\",\""
        + camB.toSimpleStdString() + "\"],\"on\":true}}");
    ASSERT_EQ(1u, m_controlCalls.size());
    EXPECT_EQ((std::vector<nx::Uuid>{camA, camB}), m_controlCalls[0].deviceIds);
    EXPECT_TRUE(m_controlCalls[0].on);
}

TEST_F(WebRtcStreamerTalk, binaryAudioReachesHandlerWithDeviceId)
{
    installTalkHandlers();
    const auto cam = nx::Uuid::createUuid();
    const std::string opus("\x01\x02\x03opus", 7);

    sendBinary(talkFrame(cam, opus));
    ASSERT_EQ(1u, m_audioCalls.size());
    EXPECT_EQ(cam, m_audioCalls[0].deviceId);
    EXPECT_EQ(opus, m_audioCalls[0].opus);
    EXPECT_TRUE(m_controlCalls.empty()); //< Audio never masquerades as a control command.
}

TEST_F(WebRtcStreamerTalk, shortBinaryFrameRaisesNoAudio)
{
    installTalkHandlers();
    sendBinary(std::string(8, '\0')); //< Fewer than the 16 deviceId bytes plus a payload.
    EXPECT_TRUE(m_audioCalls.empty());
}

TEST_F(WebRtcStreamerTalk, binaryWithoutTalkHandlersStillRunsLegacyCommand)
{
    // No installTalkHandlers(): a session that never opted into talk must keep
    // treating a binary frame as a JSON command, exactly as before this change.
    const auto cam = nx::Uuid::createUuid();
    sendBinary("{\"metadata\":true,\"deviceId\":\"" + cam.toSimpleStdString() + "\"}");
    EXPECT_TRUE(metadataEnabledFor(cam)); //< The legacy binary-as-command path still fires.
    EXPECT_TRUE(m_audioCalls.empty());
}

} // namespace nx::webrtc::test
