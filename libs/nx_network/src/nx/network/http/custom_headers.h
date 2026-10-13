// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

// TODO: #akolesnikov Most (if not all) of these strings are not relevant for nx_network and should
// be moved to vms.
namespace Qn {

static constexpr char GUID_HEADER_NAME[] = "X-guid";
static constexpr char SERVER_GUID_HEADER_NAME[] = "X-server-guid";
static constexpr char CAMERA_GUID_HEADER_NAME[] = "X-camera-guid";
static constexpr char AUTH_SESSION_HEADER_NAME[] = "X-Auth-Session";
static constexpr char USER_HOST_HEADER_NAME[] = "X-User-Host";
static constexpr char USER_AGENT_HEADER_NAME[] = "User-Agent";
static constexpr char REALM_HEADER_NAME[] = "X-Nx-Realm";
static constexpr char HA1_DIGEST_HEADER_NAME[] = "X-Nx-Digest";
static constexpr char CRYPT_SHA512_HASH_HEADER_NAME[] = "X-Nx-Crypt-Sha512";

// Flag is used to preserve compatibility with old servers.
static constexpr char RTSP_DATA_SEND_MOTION_HEADER_NAME[] = "x-send-motion";

static constexpr char EC2_RUNTIME_GUID_HEADER_NAME[] = "X-runtime-guid";
static constexpr char EC2_INTERNAL_RTP_FORMAT[] = "X-FFMPEG-RTP";

} // namespace Qn
