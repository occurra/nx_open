// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "response.h"

namespace nx::network::rest {

Response::Response(http::StatusCode::Value statusCode, http::HttpHeaders httpHeaders):
    statusCode(statusCode),
    httpHeaders(std::move(httpHeaders))
{
}

} // namespace nx::network::rest
