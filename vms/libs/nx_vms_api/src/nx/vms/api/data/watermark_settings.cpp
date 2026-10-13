// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "watermark_settings.h"

#include <nx/reflect/compare.h>
#include <nx/reflect/json.h>

namespace nx::vms::api {

bool WatermarkSettings::operator==(const WatermarkSettings& other) const
{
    return nx::reflect::equals(*this, other);
}

} // namespace nx::vms::api
