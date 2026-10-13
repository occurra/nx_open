// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QDir>
#include <QtCore/QString>

#include <nx/utils/move_only_func.h>

namespace nx::utils::debug {

using OnAboutToBlockHandler = nx::MoveOnlyFunc<void()>;

/**
 * @return The previous handler
 */
NX_UTILS_API OnAboutToBlockHandler setOnAboutToBlockHandler(OnAboutToBlockHandler handler);

NX_UTILS_API void onAboutToBlock();

} // namespace nx::utils::debug
