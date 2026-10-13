// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "types.h"

#include <nx/reflect/to_string.h>

namespace nx::sql {

DBResult::DBResult(DBResultCode code, std::string text):
    code(code),
    text(std::move(text))
{
}

bool DBResult::ok() const
{
    return code == DBResultCode::ok;
}

std::string DBResult::toString() const
{
    std::string str = nx::reflect::toString(code);
    if (!text.empty())
        str.append(". ").append(text);
    return str;
}

bool DBResult::operator==(DBResultCode right) const
{
    return code == right;
}

// const char* toString(DBResult result)
//{
//     switch (result)
//     {
//         case DBResultCode::ok:
//             return "ok";
//         case DBResultCode::statementError:
//             return "statementError";
//         case DBResultCode::ioError:
//             return "ioError";
//         case DBResult::notFound:
//             return "notFound";
//         case DBResultCode::cancelled:
//             return "cancelled";
//         case DBResult::retryLater:
//             return "retryLater";
//         case DBResult::uniqueConstraintViolation:
//             return "uniqueConstraintViolation";
//         case DBResult::connectionError:
//             return "connectionError";
//         case DBResult::logicError:
//             return "logicError";
//         case DBResult::endOfData:
//             return "endOfData";
//         default:
//             return "unknown";
//     }
// }

//-------------------------------------------------------------------------------------------------

Exception::Exception(DBResult dbResult):
    base_type(dbResult.toString()),
    m_dbResult(std::move(dbResult))
{
}

Exception::Exception(DBResultCode code, const std::string& text):
    Exception(DBResult(code, text))
{
}

DBResult Exception::dbResult() const
{
    return m_dbResult;
}

} // namespace nx::sql
