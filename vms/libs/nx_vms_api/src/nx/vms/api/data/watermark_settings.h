// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <nx/reflect/instrument.h>

namespace nx::vms::api {

struct NX_VMS_API WatermarkSettings
{
    bool useWatermark = false;

    /**%apidoc[opt]:float Frequency in range [0, 1]. */
    double frequency = 0.5;

    /**%apidoc[opt]:float Opacity in range [0, 1]. */
    double opacity = 0.3;

    bool operator==(const WatermarkSettings& other) const;
};

#define WatermarkSettings_Fields (useWatermark)(frequency)(opacity)
NX_REFLECTION_INSTRUMENT(WatermarkSettings, WatermarkSettings_Fields)

} // namespace nx::vms::api
