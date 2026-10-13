// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include <map>
#include <string>

#include <gtest/gtest.h>

#include <nx/reflect/pp_for_each.h>

namespace nx::reflect::test {

TEST(PpForEach, ForEachSimple)
{
#define FOR_EACH_TEST(_, x) +x* x

    const int result = 0 NX_PP_FOR_EACH(FOR_EACH_TEST, _, 1, 2, 3, 4);
    ASSERT_EQ(result, 1 + 2 * 2 + 3 * 3 + 4 * 4);

#undef FOR_EACH_TEST
}

TEST(PpForEach, ForEachWithData)
{
#define FOR_EACH_TEST(a, x) a += x * x;

    int result = 0;
    NX_PP_FOR_EACH(FOR_EACH_TEST, result, 1, 2, 3, 4)
    ASSERT_EQ(result, 1 + 2 * 2 + 3 * 3 + 4 * 4);

#undef FOR_EACH_TEST
}

TEST(PpForEach, ForEachComplexArguments)
{
#define FOR_EACH_TEST(_, x) {NX_PP_EXPAND x},

    const std::map<std::string, int> testMap{
        NX_PP_FOR_EACH(FOR_EACH_TEST, _, ("a", 1), ("b", 2), ("c", 3))};

    const std::map<std::string, int> controlMap{{"a", 1}, {"b", 2}, {"c", 3}};

    ASSERT_EQ(testMap, controlMap);

#undef FOR_EACH_TEST
}

} // namespace nx::reflect::test
