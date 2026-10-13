// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <cstdlib>
#include <cstring>
#include <string_view>

#include <QtCore/QDateTime>
#include <QtCore/QMap>
#include <QtCore/QString>
#include <QtCore/QStringList>

namespace nx::utils {

//! Generates random string containing only letters and digits
NX_UTILS_API std::string generateRandomName(int length);

/**
 * Converts bytes to human-readable string like 10M or 67K.
 * Rounds value to floor with the given float precision.
 * Should be used only for internal development and/or logs. All user-visible strings must be
 * generated using HumanReadable class methods (e.g. digitalSize()).
 */
NX_UTILS_API std::string bytesToString(uint64_t bytes, int precision = 4);

/**
 * Converts bytes number string (e.g., 64K) to integer.
 * K, M, G suffix are supported.
 * @return On failure returns 0 and (if not null) sets *ok to false.
 */
NX_UTILS_API uint64_t stringToBytes(std::string_view str, bool* ok = nullptr);
NX_UTILS_API uint64_t stringToBytes(std::string_view str, uint64_t defaultValue);

template<class T, class T2>
T unquote(const T& str, T2 quoteChar)
{
    int pos1 = str.startsWith(T2(quoteChar)) ? 1 : 0;
    int pos2 = str.endsWith(T2(quoteChar)) ? 1 : 0;
    return str.mid(pos1, str.length() - pos1 - pos2);
}

template<class T, class T2>
T trimAndUnquote(const T& str, T2 quoteChar)
{
    return unquote(str.trimmed(), quoteChar);
}

NX_UTILS_API QByteArray trimAndUnquote(const QByteArray& v);

NX_UTILS_API QString trimAndUnquote(const QString& v);

static const size_t BufferNpos = size_t(-1);

/**
 * Searches the first occurrence of any element of \0-terminated string toSearch in count elements
 *   of str, starting with element offset.
 * @param toSearch \0-terminated string.
 * @param count number of characters of str to check,
 *   NOT a length of searchable characters string (toSearch).
 * NOTE: following algorithms differ from stl analogue in following:
 *   they limit number of characters checked during search.
 */
template<class Str>
size_t find_first_of(
    const Str& str,
    const typename Str::value_type* toSearch,
    size_t offset = 0,
    size_t count = BufferNpos)
{
    const size_t toSearchDataSize = strlen(toSearch);
    const typename Str::value_type* strEnd =
        str.data() + (count == BufferNpos ? str.size() : (offset + count));
    for (const typename Str::value_type* curPos = str.data() + offset; curPos < strEnd; ++curPos)
    {
        if (memchr(toSearch, *curPos, toSearchDataSize))
            return curPos - str.data();
    }

    return BufferNpos;
}

template<class Str>
size_t find_first_not_of(
    const Str& str,
    const typename Str::value_type* toSearch,
    size_t offset = 0,
    size_t count = BufferNpos)
{
    const size_t toSearchDataSize = strlen(toSearch);
    const typename Str::value_type* strEnd =
        str.data() + (count == BufferNpos ? str.size() : (offset + count));
    for (const typename Str::value_type* curPos = str.data() + offset; curPos < strEnd; ++curPos)
    {
        if (memchr(toSearch, *curPos, toSearchDataSize) == NULL)
            return curPos - str.data();
    }

    return BufferNpos;
}

/**
 * Convert any buffer to hex string.
 */
NX_UTILS_API std::string toHex(
    const void* buffer,
    const int size,
    const std::string& delimeter = " ");

} // namespace nx::utils
