// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <nx/kit/ini_config.h>

// TODO: Rename and put into namespaces when nx_streaming is extracted from lib common.
struct NxStreamingIniConfig: public nx::kit::IniConfig
{
    NxStreamingIniConfig():
        nx::kit::IniConfig("nx_streaming.ini")
    {
        reload();
    }

    NX_INI_STRING(
        "",
        analyticsMetadataLogFilePrefix,
        "If not empty, metadata will be logged to this file.");

    NX_INI_FLAG(
        1,
        enableTimeCorrection,
        "Enables time correction if timestamp difference between subsequent frames is too small.");

    NX_INI_INT(
        1'000,
        resyncThresholdMs,
        "If the difference between camera time and server time is bigger than the value of this\n"
        "setting an offset between the clocks of the camera and the server is recalculated");

    NX_INI_INT(
        5'000,
        streamsSyncThresholdMs,
        "If the difference between the camera primary and secondary stream clocks is bigger\n"
        "than the value of this settings then the secondary stream time offset is recalculated");

    NX_INI_INT(
        10'000,
        forceCameraTimeThresholdMs,
        "If the difference between the camera clock and the server clock is bigger than this\n"
        "value then the server will bind the camera clock to the server one. Otherwise the\n"
        "camera clock will be used");

    NX_INI_INT(
        3600'000,
        cameraMetadataTimeThresholdMs,
        "If the difference between metadata clock received via RTSP and the server clock is bigger\n"
        "than this value then the server will bind the camera clock to the server one. Otherwise\n"
        "the camera clock will be used");

    NX_INI_INT(
        5'000,
        maxExpectedMetadataDelayMs,
        "For metadata arriving via a side channel (i.e not via the RTSP connection), when\n"
        "RTSP provides no absolute timing information, if the difference between the camera\n"
        "clock used for metadata and the server clock is bigger than this value plus\n"
        "forceCameraTimeThresholdMs then the server will bind the camera metadata clock to the\n"
        "server one. Otherwise the camera metadata clock will be used");

    NX_INI_INT(
        130,
        noUtcResyncThresholdMs,
        "resyncThresholdMs equivalent for a camera that provides no UTC time at all (no RTCP\n"
        "Sender Report and no ONVIF timestamp extension). Such a stream is bound to the server\n"
        "clock; this smaller threshold keeps that binding tight so two lanes of the same camera\n"
        "cannot drift far apart. Larger values allow more drift; smaller values re-anchor more\n"
        "often, which steps the timeline forward more frequently.");

    NX_INI_INT(
        75,
        noUtcSecondaryStreamSyncThresholdMs,
        "For a camera without UTC time, the maximum a dependent lane (the secondary stream, or\n"
        "an audio/metadata track) may differ from the primary lane's reconstructed current time\n"
        "before it is re-bound to it. Keeps overlay metadata produced on one lane aligned with\n"
        "video viewed on the other.");

    NX_INI_INT(
        1'000,
        noUtcBurstWindowMs,
        "For a camera without UTC time, the window after stream open during which the\n"
        "server-clock anchor is re-derived on every packet. A camera often dumps its buffered\n"
        "GOP backlog as a burst right after open; without this the anchor would be baked onto a\n"
        "stale backlog frame and run permanently ahead.");
};

NX_VMS_COMMON_API NxStreamingIniConfig& nxStreamingIni();
