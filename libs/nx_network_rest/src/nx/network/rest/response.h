// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include "content.h"

namespace nx::network::rest {

struct NX_NETWORK_REST_API Response
{
    nx::network::http::StatusCode::Value statusCode = nx::network::http::StatusCode::undefined;
    std::optional<Content> content;
    bool isUndefinedContentLength = false;
    nx::network::http::HttpHeaders httpHeaders;

    Response(
        nx::network::http::StatusCode::Value statusCode = nx::network::http::StatusCode::undefined,
        nx::network::http::HttpHeaders httpHeaders = {});
};

} // namespace nx::network::rest
