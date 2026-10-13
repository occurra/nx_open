// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QString>

namespace nx::utils {

class NX_UTILS_API AppInfo
{
public:
    /** Returns full VMS version including commit hash and developer build suffix. */
    static QString vmsFullVersion();
};

} // namespace nx::utils
