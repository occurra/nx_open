// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <nx/kit/ini_config.h>
#include <nx/rtp/rtp.h>

namespace nx::rtp {

struct NX_RTP_API IniConfig: public nx::kit::IniConfig
{
    IniConfig():
        nx::kit::IniConfig("nx_rtp.ini")
    {
        reload();
    }

    NX_INI_FLAG(
        1,
        rtcpTimestampSmoothing,
        "Smooth re-anchoring of media timestamps to RTCP Sender Reports, so a single jittery or\n"
        "inaccurate report (some cameras, notably Hikvision H.265 streams, emit reports whose NTP\n"
        "and RTP fields disagree by up to ~1.5s) cannot push the timestamp backward and freeze the\n"
        "decoder. Disable to map each Sender Report directly (legacy behavior).");
};

NX_RTP_API IniConfig& ini();

} // namespace nx::rtp
