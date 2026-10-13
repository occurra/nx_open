// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "string.h"

#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>

#include <QtCore/QByteArray>
#include <QtCore/QRegularExpression>

#include <nx/utils/datetime.h>
#include <nx/utils/exception.h>
#include <nx/utils/log/assert.h>
#include <nx/utils/random.h>
#include <nx/utils/std_string_utils.h>

namespace nx::utils {

std::string generateRandomName(int length)
{
    return random::generateName(length);
}

static const double kByteSuffixLimit = 1024;
static const std::vector<char> kByteSuffexes = {'K', 'M', 'G', 'T'};

// TODO: #sivanov Move out strings and logic to separate class.
std::string bytesToString(uint64_t bytes, int precision)
{
    double number = static_cast<double>(bytes);
    size_t suffix = 0;

    while (number >= kByteSuffixLimit && suffix < kByteSuffexes.size())
    {
        number /= kByteSuffixLimit;
        ++suffix;
    }

    // TODO: #akolesnikov Use std::to_chars when available on every platform.

    if (suffix == 0)
        return nx::format("%1").arg(number, 0, 'g', precision).toStdString();

    return nx::format("%1%2")
        .arg(number, 0, 'g', precision)
        .arg(kByteSuffexes[suffix - 1])
        .toStdString();
}

uint64_t stringToBytes(std::string_view str, bool* isOk)
{
    if (str.empty())
    {
        if (isOk)
            *isOk = false;
        return 0;
    }

    const auto subDouble = [isOk](std::string_view str, const auto& multi)
    {
        std::size_t endPos = 0;
        const auto result = static_cast<uint64_t>(stod(str, &endPos) * multi);
        if (isOk)
            *isOk = endPos > 0;
        return result;
    };

    for (size_t i = 0; i < kByteSuffexes.size(); ++i)
    {
        if (toupper(str.back()) == toupper(kByteSuffexes[i]))
        {
            return subDouble(
                str.substr(0, str.size() - 1),
                std::pow(kByteSuffixLimit, static_cast<double>(i + 1)));
        }
    }

    // NOTE: avoid double to be the most precise
    std::size_t endPos = 0;
    const auto result = stoull(str, &endPos);
    if (isOk)
        *isOk = endPos > 0;
    return result;
}

uint64_t stringToBytes(std::string_view str, uint64_t defaultValue)
{
    bool isOk = false;
    uint64_t value = stringToBytes(str, &isOk);
    if (isOk)
        return value;
    else
        return defaultValue;
}

QByteArray trimAndUnquote(const QByteArray& v)
{
    return trimAndUnquote(v, '\"');
}

QString trimAndUnquote(const QString& v)
{
    return trimAndUnquote(v, '\"');
}

std::string toHex(const void* buffer, const int size, const std::string& delimeter)
{
    const uint8_t* data = (const uint8_t*) buffer;
    std::stringstream stream;
    stream << std::hex;
    for (int i = 0; i < size; ++i)
    {
        stream << std::setfill('0') << std::setw(2) << (int) data[i];
        if (i + 1 < size)
            stream << delimeter;
    }
    return stream.str();
}

} // namespace nx::utils
