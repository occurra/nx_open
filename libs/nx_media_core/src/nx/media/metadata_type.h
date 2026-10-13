// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <nx/reflect/instrument.h>

/**
 * Unified metadata type enum shared between the media pipeline and AI analytics.
 *
 * Integer values 0..4 are used in the protobuf wire protocol (MetadataItem.metadata_type)
 * and MUST NOT change. Internal pipeline types use values >= 100.
 */
NX_REFLECTION_ENUM_CLASS(MetadataType,
    // AI analytics types (wire-protocol values — do not renumber).
    object    = 0,          ///< Object detections (person, vehicle, …)
    heatMap   = 1,          ///< Heat-map / sensitivity grids
    event     = 2,          ///< Prolonged or instant analytics events
    bestShot  = 3,          ///< Best frame for an event
    action    = 4,          ///< Action metadata
    embedding = 5,          ///< Edge-side enrichment result (image/text embedding)

    // Internal pipeline types (not on the wire).
    mediaStreamEvent = 101, ///< In-band stream lifecycle events
    inStream         = 102, ///< Generic in-stream metadata from container
    list             = 103  ///< All metadata of one device at one timestamp, as one packet
)

/**
 * The overlay-metadata subset that annotates video frames - object boxes, heat maps and
 * per-frame lists. Notification metadata (events, best shots, ...) is delivered to clients
 * directly and never replayed from a stream.
 */
constexpr bool isOverlayMetadata(MetadataType type)
{
    return type == MetadataType::object || type == MetadataType::heatMap
        || type == MetadataType::list;
}
