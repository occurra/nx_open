// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QString>

#include <nx/network/http/http_types.h>
#include <nx/reflect/enum_instrument.h>
#include <nx/reflect/json.h>
#include <nx/reflect/json/deserializer.h>

namespace nx::network::rest {

NX_REFLECTION_ENUM_CLASS(ErrorId,
        ok = 0,
        missingParameter = 1,
        invalidParameter = 2,
        cantProcessRequest = 3,
        forbidden = 4,
        badRequest = 5,
        internalServerError = 6,
        conflict = 7,
        notImplemented = 8,
        notFound = 9,
        unsupportedMediaType = 10,
        serviceUnavailable = 11,
        unauthorized = 12,
        sessionExpired = 13,
        sessionRequired = 14,
        sessionTruncated = 15,
        gone = 16,
        notAllowed = 17,
        serviceUnauthorized = 18,
        insufficientStorage = 19
)

/**%apidoc:object JSON object including the error status.
 * %param:string error Numeric string representation of `errorId`.
 *     %deprecated Use `errorId` instead.
 *     %value "0" ok
 *     %value "1" missingParameter
 *     %value "2" invalidParameter
 *     %value "3" cantProcessRequest
 *     %value "4" forbidden
 *     %value "5" badRequest
 *     %value "6" internalServerError
 *     %value "7" conflict
 *     %value "8" notImplemented
 *     %value "9" notFound
 *     %value "10" unsupportedMediaType
 *     %value "11" serviceUnavailable
 *     %value "12" unauthorized
 *     %value "13" sessionExpired
 *     %value "14" sessionRequired
 *     %value "15" sessionTruncated
 *     %value "16" gone
 *     %value "17" notAllowed
 *     %value "18" serviceUnauthorized
 *     %value "19" insufficientStorage
 */
struct NX_NETWORK_REST_API Result
{
    ErrorId errorId = ErrorId::ok;

    /**%apidoc Error message or an empty string. */
    QString errorString;

    Result(ErrorId error = ErrorId::ok, QString errorString = "");

    /** Maps an HTTP status onto the closest error id, falling back to badRequest. */
    static ErrorId errorFromHttpStatus(int status);
};

NX_NETWORK_REST_API nx::reflect::DeserializationResult deserialize(
    const nx::reflect::json::DeserializationContext& context,
    Result* data);

template<typename SerializationContext>
inline void serialize(SerializationContext* context, const Result& value)
{
    context->composer.startObject();
    context->composer.writeAttributeName("errorId");
    context->composer.writeString(nx::reflect::enumeration::toString(value.errorId));
    context->composer.writeAttributeName("errorString");
    context->composer.writeString(value.errorString.toStdString());
    context->composer.writeAttributeName("error");
    context->composer.writeString(std::to_string((int) value.errorId));
    context->composer.endObject(/*members*/ 3);
}

} // namespace nx::network::rest
