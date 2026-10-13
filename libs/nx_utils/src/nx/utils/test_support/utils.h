// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <stdexcept>

/**
 * Analogue of gtest's ASSERT_EQ but supports placing in non-void methods (throws on failure).
 */
#define NX_GTEST_ASSERT_EQ(expected, actual) \
    do \
    { \
        bool resultE31cf27915b242999a4e22c7757d94de = false; \
        [&]() -> void \
        { \
            ASSERT_EQ(expected, actual); \
            resultE31cf27915b242999a4e22c7757d94de = true; \
        }(); \
        if (!resultE31cf27915b242999a4e22c7757d94de) \
        { \
            throw std::runtime_error( \
                "This is work around ASSERT_* inability to be used " \
                "in non-void method. Just ignore..."); \
        } \
    } while (0)

#define NX_GTEST_ASSERT_TRUE(expected) \
    do \
    { \
        NX_GTEST_ASSERT_EQ(true, expected); \
    } while (0)
