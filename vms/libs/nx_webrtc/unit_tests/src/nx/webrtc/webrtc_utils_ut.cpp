// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include <gtest/gtest.h>

#include <nx/webrtc/webrtc_utils.h>

namespace nx::webrtc::test {

using nx::network::HostAddress;

TEST(WebrtcUtils, privateScopeV4)
{
    EXPECT_TRUE(isPrivateScope(HostAddress("10.0.0.1")));
    EXPECT_TRUE(isPrivateScope(HostAddress("172.16.0.1")));
    EXPECT_TRUE(isPrivateScope(HostAddress("172.31.255.254")));
    EXPECT_TRUE(isPrivateScope(HostAddress("192.168.1.193")));
    EXPECT_TRUE(isPrivateScope(HostAddress("100.97.241.6"))); //< CGNAT (Tailscale v4).
    EXPECT_TRUE(isPrivateScope(HostAddress("100.64.0.1")));
    EXPECT_TRUE(isPrivateScope(HostAddress("169.254.10.10"))); //< Link-local.
    EXPECT_TRUE(isPrivateScope(HostAddress("127.0.0.1")));

    EXPECT_FALSE(isPrivateScope(HostAddress("99.167.210.46")));
    EXPECT_FALSE(isPrivateScope(HostAddress("8.8.8.8")));
    EXPECT_FALSE(isPrivateScope(HostAddress("172.32.0.1"))); //< Just past 172.16/12.
    EXPECT_FALSE(isPrivateScope(HostAddress("100.128.0.1"))); //< Just past 100.64/10.
    EXPECT_FALSE(isPrivateScope(HostAddress("11.0.0.1")));
}

TEST(WebrtcUtils, privateScopeV6)
{
    // ULA fc00::/7, notably NOT only fd00:/16 - Tailscale uses fd7a:...
    EXPECT_TRUE(isPrivateScope(HostAddress("fd7a:115c:a1e0::9e3b:f107")));
    EXPECT_TRUE(isPrivateScope(HostAddress("fd00::1")));
    EXPECT_TRUE(isPrivateScope(HostAddress("fc00::1")));
    EXPECT_TRUE(isPrivateScope(HostAddress("fe80::1"))); //< Link-local.
    EXPECT_TRUE(isPrivateScope(HostAddress("::1")));

    EXPECT_FALSE(isPrivateScope(HostAddress("2600:1700:4db0:8cd0::f")));
    EXPECT_FALSE(isPrivateScope(HostAddress("2001:4860:4860::8888")));
    EXPECT_FALSE(isPrivateScope(HostAddress("fe00::1"))); //< Not in fe80::/10.
    EXPECT_FALSE(isPrivateScope(HostAddress("ff02::1"))); //< Multicast, not ULA.
}

TEST(WebrtcUtils, privateScopeNonIp)
{
    EXPECT_FALSE(isPrivateScope(HostAddress("example.com")));
    EXPECT_FALSE(isPrivateScope(HostAddress("")));
}

TEST(WebrtcUtils, sameIpV4)
{
    EXPECT_TRUE(sameIp(HostAddress("99.167.210.46"), HostAddress("99.167.210.46")));
    // v4-mapped v6 text form still equals the plain v4 form.
    EXPECT_TRUE(sameIp(HostAddress("::ffff:99.167.210.46"), HostAddress("99.167.210.46")));
    EXPECT_FALSE(sameIp(HostAddress("99.167.210.46"), HostAddress("99.167.210.47")));
}

TEST(WebrtcUtils, sameIpV6)
{
    // Same address, different text forms.
    EXPECT_TRUE(
        sameIp(HostAddress("2600:1700:4db0:8cd0:0:0:0:f"), HostAddress("2600:1700:4db0:8cd0::f")));
    EXPECT_FALSE(
        sameIp(HostAddress("2600:1700:4db0:8cd0::f"), HostAddress("2600:1700:4db0:8cd0::10")));
}

TEST(WebrtcUtils, sameIpFamilyMismatch)
{
    EXPECT_FALSE(sameIp(HostAddress("99.167.210.46"), HostAddress("2600:1700:4db0:8cd0::f")));
    EXPECT_FALSE(sameIp(HostAddress("example.com"), HostAddress("example.com")));
}

} // namespace nx::webrtc::test
