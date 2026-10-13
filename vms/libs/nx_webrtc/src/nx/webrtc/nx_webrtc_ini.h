// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <nx/kit/ini_config.h>
#include <nx/network/ssl/certificate.h>

struct Ini: public nx::kit::IniConfig
{
    Ini():
        IniConfig("nx_webrtc.ini")
    {
        reload();
    }

    NX_INI_STRING(
        "",
        turnServer,
        "Overrides the current Server's TURN server used for WebRTC relaying. Format:\n"
        "[login:password@]turn.example.com");

    NX_INI_STRING(
        "",
        webrtcIceCandidates,
        "Force to use some types of Ice Candidates during WebRTC connectivity.\n"
        "By default, use all possible types:\n"
        "udp,tcp,srflx,relay");

    NX_INI_INT(
        500,
        webrtcRelayFallbackDelayMs,
        "Delay (ms) after remote ICE candidates arrive before allocating a TURN\n"
        "relay, if no direct (host/srflx) candidate pair has succeeded by then.\n"
        "Lower = faster relay fallback but more relay allocations. A working\n"
        "direct pair validates within 1-2 RTTs (well under 500ms even\n"
        "intercontinentally), so 500ms lets direct win before we spend a relay\n"
        "allocation, while cutting ~1s of dead wait for clients that can only\n"
        "reach via relay (symmetric-NAT, IPv4-only). Raise it to trade relay\n"
        "allocations for tolerance of very slow direct-path establishment.");

    NX_INI_INT(
        250,
        webrtcDtlsInitialRetransmitMs,
        "Initial DTLS handshake retransmit timeout (ms); doubles on each retry,\n"
        "capped at 3s. OpenSSL's default of 1000ms turns a single lost handshake\n"
        "packet into a ~1s connect stall. 0 = keep the OpenSSL default.");

    NX_INI_FLAG(
        true,
        webrtcSplitHorizonHostCandidates,
        "Advertise private-scope host candidates (RFC 1918, CGNAT 100.64/10, IPv6 ULA)\n"
        "only to clients whose reflexive public IP matches this Server's own, i.e.\n"
        "clients behind the same NAT. Remote clients then never see unreachable\n"
        "private addresses, which avoids the browser's dead-candidate nomination\n"
        "stall (~1.3s in Chrome). If this Server cannot resolve its own reflexive\n"
        "address (STUN unreachable, isolated network), classification is impossible\n"
        "and all candidates are advertised immediately, as before.");

    NX_INI_FLAG(
        true,
        webrtcSrtpTransportEnableH265,
        "Enable support of H.265 for native SRTP transport.");
    NX_INI_FLAG(
        true,
        webrtcFixH264ProfileConstraints,
        "Overwrite profile level ID in the SDP to"
        "declare base and main profiles constrained, this helps streams be acceptable to Safari.");
};

Ini& ini();
