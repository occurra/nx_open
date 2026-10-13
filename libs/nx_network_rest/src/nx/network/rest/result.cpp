// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "result.h"

#include <nx/reflect/json.h>

namespace nx::network::rest {

ErrorId Result::errorFromHttpStatus(int status)
{
    using namespace nx::network::http;
    switch (status)
    {
        case StatusCode::ok:
            return ErrorId::ok;

        case StatusCode::unprocessableEntity:
            return ErrorId::cantProcessRequest;

        case StatusCode::forbidden:
            return ErrorId::forbidden;

        case StatusCode::conflict:
            return ErrorId::conflict;

        case StatusCode::notAllowed:
            return ErrorId::notAllowed;

        case StatusCode::notImplemented:
            return ErrorId::notImplemented;

        case StatusCode::notFound:
            return ErrorId::notFound;

        case StatusCode::unsupportedMediaType:
            return ErrorId::unsupportedMediaType;

        case StatusCode::serviceUnavailable:
            return ErrorId::serviceUnavailable;

        case StatusCode::internalServerError:
            return ErrorId::internalServerError;

        case StatusCode::unauthorized:
            return ErrorId::unauthorized;

        case StatusCode::insufficientStorage:
            return ErrorId::insufficientStorage;

        default:
            return ErrorId::badRequest;
    }
}

Result::Result(ErrorId error, QString errorString):
    errorId(error),
    errorString(std::move(errorString))
{
}

nx::reflect::DeserializationResult deserialize(
    const nx::reflect::json::DeserializationContext& context,
    Result* data)
{
    return nx::reflect::json_detail::Deserializer{
        nx::reflect::json::DeserializationContext{context.value},
        data}(
        nx::reflect::WrappedMemberVariableField("errorId", &Result::errorId),
        nx::reflect::WrappedMemberVariableField("errorString", &Result::errorString));
}

} // namespace nx::network::rest
