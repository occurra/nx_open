// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "db_instance_controller.h"

#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>

#include <nx/utils/log/log.h>

#include "test_support/query_executor_factory.h"

namespace nx::sql {

InstanceController::InstanceController(const ConnectionOptions& dbConnectionOptions):
    m_dbConnectionOptions(dbConnectionOptions),
    m_queryExecutor(QueryExecutorProviderFactory::instance().create(dbConnectionOptions))
{
}

bool InstanceController::initialize()
{
    NX_DEBUG(this, "Initializing DB %1", m_dbConnectionOptions.dbName);
    if (!m_queryExecutor->init())
    {
        NX_ERROR(this, "Failed to open connection to DB %1", m_dbConnectionOptions.dbName);
        return false;
    }

    NX_DEBUG(this, "Configuring DB %1", m_dbConnectionOptions.dbName);
    if (!configureDb())
    {
        NX_ERROR(this, "Failed to tune DB %1", m_dbConnectionOptions.dbName);
        if (m_dbConnectionOptions.failOnDbTuneError)
            return false;
    }

    NX_DEBUG(this, nx::format("DB initialized"));

    return true;
}

void InstanceController::stop()
{
    m_queryExecutor->pleaseStopSync();
}

AbstractAsyncSqlQueryExecutor& InstanceController::queryExecutor()
{
    return *m_queryExecutor;
}

const AbstractAsyncSqlQueryExecutor& InstanceController::queryExecutor() const
{
    return *m_queryExecutor;
}

const StatisticsCollector& InstanceController::statisticsCollector() const
{
    return m_queryExecutor->statisticsCollector();
}

bool InstanceController::configureDb()
{
    using namespace std::placeholders;

    std::promise<DBResult> cacheFilledPromise;
    auto future = cacheFilledPromise.get_future();

    m_queryExecutor->executeUpdateWithoutTran(
        &InstanceController::configureDbInstance,
        [&](DBResult dbResult)
        {
            cacheFilledPromise.set_value(dbResult);
        });

    return future.get() == DBResultCode::ok;
}

DBResult InstanceController::configureDbInstance(QueryContext* queryContext)
{
    auto enableAutoVacuumQuery = queryContext->connection()->createQuery();
    enableAutoVacuumQuery->prepare("PRAGMA auto_vacuum = 1");
    try
    {
        enableAutoVacuumQuery->exec();
    }
    catch (const Exception& e)
    {
        NX_WARNING(NX_SCOPE_TAG, "Failed to enable auto vacuum mode. %1", e.what());
        return e.dbResult();
    }

    auto enableWalQuery = queryContext->connection()->createQuery();
    enableWalQuery->prepare("PRAGMA journal_mode = WAL");
    try
    {
        enableWalQuery->exec();
    }
    catch (const Exception& e)
    {
        NX_WARNING(NX_SCOPE_TAG, "Failed to enable WAL mode. %1", e.what());
        return e.dbResult();
    }

    auto enableFKQuery = queryContext->connection()->createQuery();
    enableFKQuery->prepare("PRAGMA foreign_keys = ON");
    try
    {
        enableFKQuery->exec();
    }
    catch (const Exception& e)
    {
        NX_WARNING(NX_SCOPE_TAG, "Failed to enable foreign keys. %1", e.what());
        return e.dbResult();
    }

    // QSqlQuery setLockingModeQuery(*connection);
    // setLockingModeQuery.prepare("PRAGMA locking_mode = NORMAL");
    // if (!setLockingModeQuery.exec())
    //{
    //     NX_WARNING(this, nx::format("sqlite configure. Failed to set locking mode. %1")
    //         .arg(setLockingModeQuery.lastError().text()));
    //     return DBResultCode::ioError;
    // }

    return DBResultCode::ok;
}

} // namespace nx::sql
