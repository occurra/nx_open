// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "log_main.h"

#include <atomic>

#include <nx/utils/nx_utils_ini.h>
#include <nx/utils/time.h>

#include "logger_collection.h"

namespace nx::log {

namespace detail {

/*static*/ bool LevelReducer::s_isEnabled = true;

std::pair<Level, bool /*isOnLimit*/> LevelReducer::nextLevel()
{
    if (!s_isEnabled || (m_level > Level::info))
        return {m_level, /*isOnLimit*/ false};

    const auto limit = (uint32_t) nx::utils::ini().logLevelReducerPassLimit;
    const auto windowSizeS = (uint32_t) nx::utils::ini().logLevelReducerWindowSizeS;

    const auto nowS = (uint32_t) std::chrono::duration_cast<std::chrono::seconds>(
                          nx::utils::monotonicTime().time_since_epoch())
                          .count();

    const auto windowStartS = m_windowStartS.load();
    if (m_passCount == 0 || windowStartS + windowSizeS <= nowS || windowStartS > nowS)
    {
        // It is possible to have a race on these 2 atomics and actual log writing, but it
        // would only affect the number of records on each level and their order.
        m_windowStartS = nowS;
        m_passCount = 0;
    }

    const auto newCount = ++m_passCount;
    return {
        (newCount <= limit) ? m_level : Level::debug,
        /*isOnLimit*/ (newCount == limit)};
}

} // namespace detail

namespace {

LoggerCollection* loggerCollection()
{
    return LoggerCollection::instance();
}

static std::atomic<bool> s_isConfigurationLocked = false;

} // namespace

std::shared_ptr<AbstractLogger> mainLogger()
{
    return loggerCollection()->main();
}

bool setMainLogger(std::unique_ptr<AbstractLogger> logger)
{
    if (s_isConfigurationLocked)
        return false;

    loggerCollection()->setMainLogger(std::move(logger));
    return true;
}

bool addLogger(std::unique_ptr<AbstractLogger> logger, bool writeLogHeader)
{
    if (s_isConfigurationLocked)
        return false;

    if (writeLogHeader)
        logger->writeLogHeader();

    loggerCollection()->add(std::move(logger));
    return true;
}

void lockConfiguration()
{
    s_isConfigurationLocked = true;
}

void unlockConfiguration()
{
    s_isConfigurationLocked = false;
}

std::shared_ptr<AbstractLogger> getLogger(const Tag& tag)
{
    return loggerCollection()->get(tag, /*exactMatch*/ false);
}

std::shared_ptr<AbstractLogger> getExactLogger(const Tag& tag)
{
    return loggerCollection()->get(tag, /*exactMatch*/ true);
}

void removeLoggers(const std::set<Filter>& filters)
{
    loggerCollection()->remove(filters);
}

Level maxLevel()
{
    return loggerCollection()->maxLevel();
}

bool isToBeLogged(Level level, const Tag& tag)
{
    const auto& logger = getLogger(tag);
    if (!logger)
        return true;
    return logger->isToBeLogged(level, tag);
}

bool showPasswords()
{
    return nx::utils::ini().showPasswordsInLogs;
}

} // namespace nx::log
