// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <chrono>

#include <QtCore/QObject>
#include <QtCore/QString>

#include <nx/utils/property_storage/storage.h>
#include <nx/utils/value_cache.h>

namespace nx::vms::common {

using namespace std::literals::chrono_literals;

class NX_VMS_COMMON_API SystemSettings: public nx::utils::property_storage::Storage
{
    Q_OBJECT

public:
    SystemSettings();

    Property<std::chrono::milliseconds> rtpFrameTimeoutMs{
        this,
        "rtpFrameTimeoutMs",
        10s,
        "RTP timeout (milliseconds)."};

    Property<int> maxRtpRetryCount{
        this,
        "maxRtpRetryCount",
        6,
        "Maximum number of consecutive RTP errors before the server reconnects the RTSP session."};

    Property<QString> supportedOrigins{
        this,
        "supportedOrigins",
        "*",
        "HTTP header: Access-Control-Allow-Origin."};

    Property<bool> supportedOriginCredentials{
        this,
        "supportedOriginCredentials",
        false,
        "HTTP header: Access-Control-Allow-Credentials."};

    Property<std::chrono::seconds> webrtcSessionTimeout{
        this,
        "mediaSendTimeoutS",
        30s,
        "Reap delay for a WebRTC session with no live peer. Applies both to an "
        "idle session (the clock runs while no ICE transport is bound - before "
        "DTLS completes and after a peer drops) and to an actively-streaming "
        "session that has received no inbound packet for this long (peer "
        "liveness: catches a hung browser tab or a silently-dropped relay path "
        "that the ICE consent checks do not). Must exceed expected gather + DTLS "
        "time, the ICE recovery window, and the peer's consent/RTCP interval."};

public:
    std::string makeServerHeader() const;

private:
    Property<QString> serverHeader{
        this,
        "serverHeader",
        "$vmsName/$vmsVersion",
        "HTTP header: Server, supported variables: $vmsName, $vmsVersion, $company, $compatibility."};

private:
    nx::utils::CachedValue<std::string> m_serverHeaderCache;
};

} // namespace nx::vms::common
