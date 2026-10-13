// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "rtp_ini.h"

namespace nx::rtp {

IniConfig& ini()
{
    static IniConfig instance;
    return instance;
}

} // namespace nx::rtp
