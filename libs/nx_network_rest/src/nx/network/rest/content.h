// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <nx/network/http/http_types.h>

namespace nx::network::rest {

/** Body of an HTTP message together with the content type describing it. */
struct Content
{
    nx::network::http::header::ContentType type;
    QByteArray body;
};

} // namespace nx::network::rest
