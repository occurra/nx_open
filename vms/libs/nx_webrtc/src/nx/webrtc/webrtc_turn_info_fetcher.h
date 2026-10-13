// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <functional>
#include <optional>

#include <nx/utils/thread/mutex.h>

#include "webrtc_utils.h"

namespace nx::webrtc {

class NX_WEBRTC_API TurnInfoFetcher
{
public:
    /// Returns current TURN credentials, or std::nullopt if none are available
    /// (fetch failed / no cloud configured). Supplied by the media server.
    using CredentialProvider = std::function<std::optional<TurnServerInfo>()>;

public:
    TurnInfoFetcher(const std::string realm, const std::string cloudHost);
    ~TurnInfoFetcher();

    void setCredentialProvider(CredentialProvider provider);

    /// Resolution order:
    ///   1. ini() static override (dev/local), if set;
    ///   2. injected credential provider, if set and it returns a value;
    ///   3. bare server address (no creds).
    TurnServerInfo turnServerInfo() const;

private:
    mutable nx::Mutex m_mutex;
    TurnServerInfo m_turnInfo; ///< Address + ini override creds.
    bool m_iniOverrideHasCreds = false; ///< True if ini supplied user/password.
    CredentialProvider m_credentialProvider;
};

} // namespace nx::webrtc
