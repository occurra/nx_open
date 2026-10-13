// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "webrtc_turn_info_fetcher.h"

#include <nx/branding.h>
#include <nx/network/socket_common.h>

#include "nx_webrtc_ini.h"

namespace nx::webrtc {

namespace {

// Standard STUN/TURN port. SocketAddress parses a bare hostname with port 0,
// which connectAsync() cannot dial, so default to this when none is given.
static constexpr std::uint16_t kDefaultTurnPort = 3478;

} // namespace

TurnInfoFetcher::TurnInfoFetcher(const std::string realm, const std::string cloudHost)
{
    m_turnInfo.realm = realm;
    m_turnInfo.longTermAuth = true;

    const std::string turnServerIni = ini().turnServer;
    nx::network::SocketAddress address;
    auto atPos = turnServerIni.rfind('@');
    if (atPos != std::string::npos)
    {
        address = nx::network::SocketAddress(turnServerIni.substr(atPos + 1));
        auto colonPos = turnServerIni.find(':');
        if (colonPos != std::string::npos && colonPos < atPos)
        {
            m_turnInfo.user = turnServerIni.substr(0, colonPos);
            m_turnInfo.password = turnServerIni.substr(colonPos + 1, atPos - colonPos - 1);
            m_iniOverrideHasCreds = true;
        }
    }
    else if (!turnServerIni.empty())
    {
        address = nx::network::SocketAddress(turnServerIni);
    }
    else if (!cloudHost.empty())
    {
        address = nx::network::SocketAddress("turn." + cloudHost);
    }
    else
    {
        return; //< No cloud: sessions stay STUN-only.
    }

    // A hostname without an explicit ":port" leaves port == 0; apply the
    // standard TURN port so connectAsync() has a real endpoint to dial.
    if (address.port == 0)
        address.port = kDefaultTurnPort;

    // The ini override yields a single endpoint; default to UDP, which is the
    // transport the deployment's TURN server answers on.
    m_turnInfo.endpoints.push_back({address, nx::network::TransportProtocol::udp});
}

TurnInfoFetcher::~TurnInfoFetcher() = default;

void TurnInfoFetcher::setCredentialProvider(CredentialProvider provider)
{
    NX_MUTEX_LOCKER lk(&m_mutex);
    m_credentialProvider = std::move(provider);
}

TurnServerInfo TurnInfoFetcher::turnServerInfo() const
{
    CredentialProvider provider;
    {
        NX_MUTEX_LOCKER lk(&m_mutex);
        if (m_iniOverrideHasCreds)
            return m_turnInfo;
        provider = m_credentialProvider;
    }
    // Invoke the provider without holding m_mutex: it may block on network I/O,
    // and m_turnInfo is immutable after construction so the fallback read is safe.
    if (provider)
    {
        if (auto info = provider())
            return *info;
    }
    return m_turnInfo;
}

} // namespace nx::webrtc
