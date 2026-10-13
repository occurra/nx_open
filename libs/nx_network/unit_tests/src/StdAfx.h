// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

// This header includes <windows.h> and _must_ be included before everything
#include <nx/utils/system_network_headers.h>
/////////////////////////////////////////////////////////////////////////////////////////

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QtCore/QSharedPointer>

#include <nx/network/aio/basic_pollable.h>
#include <nx/network/http/http_types.h>
#include <nx/network/socket_global.h>
#include <nx/utils/buffer.h>
#include <nx/utils/log/log.h>
#include <nx/utils/move_only_func.h>
