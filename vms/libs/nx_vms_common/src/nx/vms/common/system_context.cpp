// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "system_context.h"

#include <nx/metric/metrics_storage.h>
#include <nx/network/http/auth_tools.h>
#include <nx/vms/common/system_settings.h>

namespace nx::vms::common {

struct SystemContext::Private
{
    const nx::Uuid peerId;

    std::unique_ptr<SystemSettings> systemSettings;
    std::shared_ptr<nx::metric::Storage> metrics;
};

SystemContext::SystemContext(nx::Uuid peerId, QObject* parent):
    QObject(parent),
    d(new Private{.peerId = peerId})
{
    d->systemSettings = std::make_unique<SystemSettings>();

    d->metrics = std::make_shared<nx::metric::Storage>();
}

SystemContext::~SystemContext()
{
}

const nx::Uuid& SystemContext::peerId() const
{
    return d->peerId;
}

SystemSettings* SystemContext::systemSettings() const
{
    return d->systemSettings.get();
}

std::shared_ptr<nx::metric::Storage> SystemContext::metrics() const
{
    return d->metrics;
}

} // namespace nx::vms::common
