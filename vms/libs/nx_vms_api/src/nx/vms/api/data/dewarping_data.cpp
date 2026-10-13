// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "dewarping_data.h"

#include <nx/reflect/compare.h>
#include <nx/reflect/json.h>

namespace nx::vms::api::dewarping {

namespace {

static const int kPanoLow = 1;
static const int kPanoNormal = 2;
static const int kPanoWide = 4;

static const QList<int> kHorizontalPano({kPanoLow, kPanoNormal});
static const QList<int> kVerticalPano({kPanoLow, kPanoNormal, kPanoWide});
static const QList<int> k360Pano({kPanoLow, kPanoNormal, kPanoWide});

} // namespace

const QList<int>& MediaData::allowedPanoFactorValues() const
{
    return is360VR() ? k360Pano : allowedPanoFactorValues(viewMode);
}

const QList<int>& MediaData::allowedPanoFactorValues(FisheyeCameraMount mode)
{
    return (mode == FisheyeCameraMount::wall) ? kHorizontalPano : kVerticalPano;
}

bool MediaData::is360VR() const
{
    return is360VR(cameraProjection);
}

bool MediaData::is360VR(CameraProjection projection)
{
    return projection == CameraProjection::equirectangular360;
}

bool MediaData::isFisheye() const
{
    return isFisheye(cameraProjection);
}

bool MediaData::isFisheye(CameraProjection projection)
{
    return !is360VR(projection);
}

bool MediaData::operator==(const MediaData& other) const
{
    return nx::reflect::equals(*this, other);
}

//-------------------------------------------------------------------------------------------------
// ViewData

bool ViewData::operator==(const ViewData& other) const
{
    return nx::reflect::equals(*this, other);
}

} // namespace nx::vms::api::dewarping
