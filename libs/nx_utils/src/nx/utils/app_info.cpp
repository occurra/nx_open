// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "app_info.h"

#include <nx/branding.h>
#include <nx/build_info.h>

namespace nx::utils {

using namespace nx::build_info;

QString AppInfo::vmsFullVersion()
{
    static const QString kFullVersion =
        QString("%1-%2-%3").arg(vmsVersion()).arg(revision()).arg(developerBuildSuffix());

    return kFullVersion;
}

} // namespace nx::utils
