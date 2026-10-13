// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QFlags>

#include <nx/reflect/enum_instrument.h>

namespace nx::vms::api {

NX_REFLECTION_ENUM(
    ResourceStatus,
    /** The Server or Device is inaccessible. */
    offline = 0,

    /** The Device does not have correct credentials. */
    unauthorized = 1,

    /** The Device is operating normally. */
    online = 2,

    /** The Camera is online and recording. */
    recording = 3);

inline bool isOnline(ResourceStatus status)
{
    return status == ResourceStatus::online || status == ResourceStatus::recording;
}

enum class StreamQuality
{
    lowest = 0,
    low = 1,
    normal = 2,
    high = 3,
    highest = 4,
    preset = 5,
    undefined = 6,
    rapidReview = 7,
};

NX_REFLECTION_ENUM_CLASS(StreamDataFilter, media = 1 << 0, motion = 1 << 1, objects = 1 << 2)

Q_DECLARE_FLAGS(StreamDataFilters, StreamDataFilter)
Q_DECLARE_OPERATORS_FOR_FLAGS(StreamDataFilters)

} // namespace nx::vms::api
