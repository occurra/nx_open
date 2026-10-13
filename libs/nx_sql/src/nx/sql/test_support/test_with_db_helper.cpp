// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "test_with_db_helper.h"

#include <optional>

#include <QtCore/QDir>

#include <nx/utils/log/format.h>
#include <nx/utils/test_support/test_options.h>

namespace nx::sql::test {

std::optional<nx::sql::ConnectionOptions> TestWithDbHelper::sDbConnectionOptions;

TestWithDbHelper::TestWithDbHelper(QString tmpDir):
    utils::test::TestWithTemporaryDirectory(tmpDir)
{
    if (sDbConnectionOptions)
        m_dbConnectionOptions = *sDbConnectionOptions;

    // SqLite doesn't support multi-thread access.
    m_dbConnectionOptions.maxConnectionCount = 1;

    if (m_dbConnectionOptions.dbName.isEmpty())
    {
        static std::atomic<int> sequence{0};
        m_dbConnectionOptions.dbName =
            QString("%1/%2").arg(testDataDir()).arg(QString("%1.db").arg(++sequence));
    }
}

TestWithDbHelper::~TestWithDbHelper()
{
}

const nx::sql::ConnectionOptions& TestWithDbHelper::dbConnectionOptions() const
{
    return m_dbConnectionOptions;
}

nx::sql::ConnectionOptions& TestWithDbHelper::dbConnectionOptions()
{
    return m_dbConnectionOptions;
}

void TestWithDbHelper::setDbConnectionOptions(const nx::sql::ConnectionOptions& connectionOptions)
{
    sDbConnectionOptions = connectionOptions;
}

} // namespace nx::sql::test
