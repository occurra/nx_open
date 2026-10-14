// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "supplemental_enhancement_information.h"

#include <nx/codec/h264/common.h>
#include <nx/codec/sei_common.h>

namespace nx::media::h264 {

std::expected<sei::SeiUserData, std::string> parseSeiUserData(std::span<const uint8_t> naluEbsp)
{
    return sei::parseSeiNalUnit(naluEbsp, kNalUnitHeaderLength);
}

} // namespace nx::media::h264
