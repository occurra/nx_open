// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "metatypes.h"

#include <mutex>

#include <QtCore/QList>

#include "data/dewarping_data.h"
#include "data/image_correction_data.h"
#include "types/resource_types.h"
#include "types/rtp_types.h"
#include "types/stream_index.h"

namespace nx::vms::api {

void initializeMetatypesInternal()
{
    qRegisterMetaType<dewarping::ViewData>();
    qRegisterMetaType<dewarping::MediaData>();
    qRegisterMetaType<dewarping::CameraProjection>();
    qRegisterMetaType<dewarping::FisheyeCameraMount>();
    qRegisterMetaType<dewarping::ViewProjection>();
    qRegisterMetaType<ImageCorrectionData>();
    qRegisterMetaType<StreamIndex>();
    qRegisterMetaType<ResourceStatus>();
    qRegisterMetaType<RtpTransportType>();
};

void Metatypes::initialize()
{
    static std::once_flag initialized;
    std::call_once(initialized, &initializeMetatypesInternal);
}

} // namespace nx::vms::api
