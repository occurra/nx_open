// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include <nx/network/stun/message.h>
#include <nx/network/stun/message_parser.h>
#include <nx/network/stun/stun_attributes.h>
#include <nx/webrtc/webrtc_ice.h>

namespace nx::webrtc::test {

namespace stun = nx::network::stun;

namespace {

stun::Message parseStunMessage(const nx::Buffer& serialized)
{
    stun::Message message;
    stun::MessageParser parser;
    parser.setFingerprintRequired(true);
    parser.setMessage(&message);
    size_t parsed = 0;
    EXPECT_EQ(parser.parse(serialized, &parsed), nx::network::server::ParserState::done);
    return message;
}

std::uint64_t decodeBigEndian64(const nx::Buffer& buffer)
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < buffer.size(); ++i)
        value = (value << 8) | static_cast<std::uint8_t>(buffer[i]);
    return value;
}

} // namespace

// A connectivity-check request the media_server sends to the browser must be
// verifiable by the browser, which keys MESSAGE-INTEGRITY with *its own*
// (remote) ice-pwd — not the server's local pwd. Signing with the local pwd
// (the prior bug) makes every server-initiated check silently rejected.
TEST(WebRtcIceBindingRequest, IntegrityIsKeyedWithRemotePwd)
{
    const std::string remoteUfrag = "browserUfrag";
    const std::string localUfrag = "serverUfrag";
    const std::string remotePwd = "browserPasswordSecret01";
    const std::string localPwd = "serverPasswordSecret02";

    const auto serialized = buildControlledBindingRequest(
        remoteUfrag,
        localUfrag,
        remotePwd,
        /*priority*/ 12345,
        /*tieBreaker*/ 0xABCDull);

    const auto message = parseStunMessage(serialized);
    const std::string username = remoteUfrag + ":" + localUfrag;

    EXPECT_TRUE(message.verifyIntegrity(username, remotePwd));
    EXPECT_FALSE(message.verifyIntegrity(username, localPwd));
}

// A full-ICE agent in the controlled role must tag its checks with
// ICE-CONTROLLED carrying its 8-byte tie-breaker (network/big-endian byte
// order), and must not also send ICE-CONTROLLING.
TEST(WebRtcIceBindingRequest, CarriesIceControlledTieBreaker)
{
    const std::uint64_t tieBreaker = 0x0102030405060708ull;

    const auto serialized = buildControlledBindingRequest(
        "browserUfrag",
        "serverUfrag",
        "pwd",
        /*priority*/ 1,
        tieBreaker);

    const auto message = parseStunMessage(serialized);

    auto* controlled = message.getAttribute<stun::attrs::IceControlled>();
    ASSERT_NE(controlled, nullptr);
    EXPECT_EQ(message.getAttribute<stun::attrs::IceControlling>(), nullptr);

    const nx::Buffer& tieBreakerBytes = controlled->getBuffer();
    ASSERT_EQ(tieBreakerBytes.size(), 8u);
    EXPECT_EQ(decodeBigEndian64(tieBreakerBytes), tieBreaker);
}

// The request carries the candidate PRIORITY the peer needs to compute the
// pair priority for its check list.
TEST(WebRtcIceBindingRequest, CarriesPriority)
{
    const int priority = 0x6E001234;

    const auto serialized = buildControlledBindingRequest(
        "browserUfrag",
        "serverUfrag",
        "pwd",
        priority,
        /*tieBreaker*/ 1ull);

    const auto message = parseStunMessage(serialized);

    auto* priorityAttr = message.getAttribute<stun::attrs::Priority>();
    ASSERT_NE(priorityAttr, nullptr);
    EXPECT_EQ(priorityAttr->value(), priority);
}

} // namespace nx::webrtc::test
