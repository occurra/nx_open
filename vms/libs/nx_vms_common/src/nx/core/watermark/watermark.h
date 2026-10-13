// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <nx/reflect/instrument.h>
#include <nx/vms/api/data/watermark_settings.h>

namespace nx {
namespace core {

struct Watermark
{
    vms::api::WatermarkSettings settings;
    QString text;

    bool operator==(const Watermark& other) const = default;

    bool visible() const { return settings.useWatermark && !text.isEmpty(); };
};
#define Watermark_Fields (settings)(text)
NX_REFLECTION_INSTRUMENT(Watermark, Watermark_Fields)

} // namespace core
} // namespace nx
