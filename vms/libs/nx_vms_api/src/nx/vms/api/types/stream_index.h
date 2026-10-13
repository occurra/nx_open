// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <nx/reflect/enum_instrument.h>

namespace nx::vms::api {

/**%apidoc Index of the stream that is requested from the camera. */
NX_REFLECTION_ENUM_CLASS(StreamIndex,
    /** High-resolution stream. */
    primary = 0,

    /** Low-resolution stream. */
    secondary = 1
);

StreamIndex NX_VMS_API oppositeStreamIndex(StreamIndex streamIndex);

} // namespace nx::vms::api
