// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <cstdint>

#include <nx/utils/uuid.h>

namespace nx::webrtc {

/** One camera's settled playback position after a frame step (epoch microseconds). */
struct CameraPosition
{
    nx::Uuid deviceId;
    int64_t timestampUs = 0;
};

} // namespace nx::webrtc
