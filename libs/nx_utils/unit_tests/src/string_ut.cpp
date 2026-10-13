// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include <array>
#include <cstring>
#include <functional>
#include <list>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include <QtCore/QList>
#include <QtCore/QVector>

#include <nx/utils/string.h>

namespace nx::utils::test {

//-------------------------------------------------------------------------------------------------
// bytes <-> string

TEST(StringAndBytes, stringToBytes)
{
    ASSERT_EQ(123456, stringToBytes("123456"));
    ASSERT_EQ(123 * 1024, stringToBytes("123K"));
    ASSERT_EQ(123 * 1024, stringToBytes("123k"));

    ASSERT_EQ(123ULL << 20, stringToBytes("123m"));
    ASSERT_EQ(123ULL << 30, stringToBytes("123g"));
    ASSERT_EQ(123ULL << 40, stringToBytes("123t"));

    bool isOk = false;
    stringToBytes("123456", &isOk);
    ASSERT_TRUE(isOk);

    stringToBytes("aaa", &isOk);
    ASSERT_FALSE(isOk);

    stringToBytes("", &isOk);
    ASSERT_FALSE(isOk);
}

TEST(StringAndBytes, bytesToString)
{
    ASSERT_EQ("1K", bytesToString(1ULL << 10));
    ASSERT_EQ("1M", bytesToString(1ULL << 20));
    ASSERT_EQ("1G", bytesToString(1ULL << 30));
    ASSERT_EQ("1T", bytesToString(1ULL << 40));

    ASSERT_EQ("512G", bytesToString((1ULL << 40) / 2));
    ASSERT_EQ("1.5T", bytesToString((1ULL << 40) * 3 / 2, 2));
}

TEST(String, trimAndUnquote)
{
    ASSERT_EQ("foo", nx::utils::trimAndUnquote(QString("\"foo\"")));
    ASSERT_EQ("foo", nx::utils::trimAndUnquote(QString("foo")));
    ASSERT_EQ("foo", nx::utils::trimAndUnquote(QString("\"foo")));
    ASSERT_EQ("foo", nx::utils::trimAndUnquote(QString("foo\"")));

    ASSERT_EQ("", nx::utils::trimAndUnquote(QString()));
    ASSERT_EQ("", nx::utils::trimAndUnquote(QString("\"\"")));
    ASSERT_EQ("", nx::utils::trimAndUnquote(QString("\"")));
}

} // namespace nx::utils::test
