// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "debug.h"

#if defined(Q_OS_WIN)
    #include <windows.h>
#else
    #include <unistd.h>
#endif

#include <QtCore/QCoreApplication>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>

namespace nx::utils::debug {

static OnAboutToBlockHandler onAboutToBlockHandler;

OnAboutToBlockHandler setOnAboutToBlockHandler(OnAboutToBlockHandler handler)
{
    return std::exchange(onAboutToBlockHandler, std::move(handler));
}

void onAboutToBlock()
{
    if (onAboutToBlockHandler)
        onAboutToBlockHandler();
}

} // namespace nx::utils::debug
