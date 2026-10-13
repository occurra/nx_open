// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "system_settings.h"

#include <QtCore/QStandardPaths>

#include <nx/branding.h>
#include <nx/build_info.h>
#include <nx/network/http/http_types.h>
#include <nx/utils/property_storage/filesystem_backend.h>

namespace {

std::string makeServerHeaderValue(QString value)
{
    return value.replace("$vmsName", nx::branding::company())
        .replace("$vmsVersion", nx::build_info::vmsVersion())
        .replace("$company", nx::branding::company())
        .replace(
            "$compatibility",
            QString::fromStdString(nx::network::http::compatibilityServerName()))
        .toStdString();
}

} // namespace

namespace nx::vms::common {

SystemSettings::SystemSettings():
    Storage(new nx::utils::property_storage::FileSystemBackend(
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/settings")),
    m_serverHeaderCache(
        [this]
        {
            return makeServerHeaderValue(serverHeader());
        })
{
    load();
    generateDocumentation("Local Settings", "Local Server Settings.");

    connect(
        &serverHeader,
        &BaseProperty::changed,
        this,
        [this](BaseProperty* /*property*/)
        {
            m_serverHeaderCache.reset();
        });
}

std::string SystemSettings::makeServerHeader() const
{
    return m_serverHeaderCache.get();
}

} // namespace nx::vms::common
