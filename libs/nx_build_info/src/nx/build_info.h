// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QString>

namespace nx::build_info {

/** Suffix of the full version and the distribution names: "-dev" for a developer build. */
NX_BUILD_INFO_API QString developerBuildSuffix();

/** Current repository revision id. */
NX_BUILD_INFO_API QString revision();

/** Current release version of the VMS application suite. */
NX_BUILD_INFO_API QString vmsVersion();

/** Build numbers are through for all projects. */
NX_BUILD_INFO_API int buildNumber();

NX_BUILD_INFO_API QString applicationPlatform();

NX_BUILD_INFO_API bool isArm();

NX_BUILD_INFO_API bool isWindows();
NX_BUILD_INFO_API bool isMacOsX();

} // namespace nx::build_info
