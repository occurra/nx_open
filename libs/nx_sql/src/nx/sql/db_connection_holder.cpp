// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "db_connection_holder.h"

#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>

#include <nx/utils/log/log.h>
#include <nx/utils/uuid.h>

#include "db_connection_factory.h"
#include "db_statistics_collector.h"

namespace nx::sql {

DbConnectionHolder::DbConnectionHolder(
    const ConnectionOptions& connectionOptions,
    std::unique_ptr<AbstractDbConnection> connection):
    m_connectionOptions(connectionOptions),
    m_connection(std::move(connection))
{
    if (!m_connection)
        m_connection = DbConnectionFactory::instance().create(connectionOptions);
}

DbConnectionHolder::~DbConnectionHolder()
{
    close();
}

const ConnectionOptions& DbConnectionHolder::connectionOptions() const
{
    return m_connectionOptions;
}

void DbConnectionHolder::setStatisticCollector(StatisticsCollector* statisticsCollector)
{
    m_connection->setStatisticsCollector(statisticsCollector);
}

bool DbConnectionHolder::open()
{
    if (!m_connection->open())
    {
        NX_WARNING(
            this,
            "Failed to establish connection to DB %1. %2",
            connectionOptions().dbName,
            m_connection->lastError());
        return false;
    }

    if (!tuneConnection())
    {
        close();
        return false;
    }

    return true;
}

AbstractDbConnection* DbConnectionHolder::dbConnection()
{
    return m_connection.get();
}

void DbConnectionHolder::close()
{
    m_connection->close();
}

std::shared_ptr<nx::sql::QueryContext> DbConnectionHolder::begin()
{
    return createNewTran();
}

DBResult DbConnectionHolder::lastError() const
{
    return m_connection->lastError();
}

bool DbConnectionHolder::tuneConnection()
{
    auto query = m_connection->createQuery();
    query->prepare("PRAGMA foreign_keys = ON");
    try
    {
        query->exec();
    }
    catch (const Exception& e)
    {
        NX_WARNING(this, "Failed to enable foreign keys. %1", e.what());
        return false;
    }
    return true;
}

std::shared_ptr<nx::sql::QueryContext> DbConnectionHolder::createNewTran()
{
    auto deleter = [](nx::sql::QueryContext* queryContext)
    {
        if (queryContext->transaction()->isActive())
            queryContext->transaction()->rollback();
        delete queryContext->transaction();
        delete queryContext;
    };

    auto transaction = std::make_unique<nx::sql::Transaction>(dbConnection());
    if (transaction->begin() != nx::sql::DBResultCode::ok)
        return nullptr;
    return std::shared_ptr<nx::sql::QueryContext>(
        new nx::sql::QueryContext(dbConnection(), transaction.release()),
        deleter);
}

} // namespace nx::sql
