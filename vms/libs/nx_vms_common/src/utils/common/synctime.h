// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <chrono>

#include <QtCore/QDateTime>

/**
 * Legacy class which always return local OS time now.
 */
class NX_VMS_COMMON_API QnSyncTime final
{
public:
    static inline qint64 currentUSecsSinceEpoch()
    {
        // system_clock::now() reads GetSystemTimePreciseAsFileTime on Windows and so stays
        // sub-microsecond. QDateTime::currentMSecsSinceEpoch() is truncated to whole milliseconds
        // and only advances at the global timer resolution (15.6 ms by default on Windows), too
        // coarse for binding an RTP stream to the server clock.
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    static inline std::chrono::microseconds currentTimePoint()
    {
        return std::chrono::microseconds(currentUSecsSinceEpoch());
    }
};
