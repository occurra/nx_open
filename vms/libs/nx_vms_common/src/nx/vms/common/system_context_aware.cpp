// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "system_context_aware.h"

#include <QtCore/QObject>

#include <nx/utils/log/assert.h>
#include <nx/utils/log/log.h>

#include "system_context.h"

namespace nx::vms::common {

struct SystemContextAware::Private
{
    mutable QPointer<SystemContext> systemContext;
};

SystemContextAware::SystemContextAware(SystemContext* systemContext):
    d(new Private{.systemContext = systemContext})
{
    NX_CRITICAL(systemContext);
    d->systemContext = systemContext;

    if (auto qobject = dynamic_cast<const QObject*>(this))
    {
        QObject::connect(
            systemContext,
            &QObject::destroyed,
            qobject,
            []()
            {
                NX_ASSERT(
                    false,
                    "Context-aware object must be destroyed before the corresponding context is.");
            });
    }
}

SystemContextAware::~SystemContextAware()
{
    // Make sure context was not ever initialized or still exists.
    NX_ASSERT(
        d->systemContext,
        "Context-aware object must be destroyed before the corresponding context is.");
}

SystemContext* SystemContextAware::systemContext() const
{
    return d->systemContext.data();
}

nx::Uuid SystemContextAware::peerId() const
{
    return systemContext()->peerId();
}

SystemSettings* SystemContextAware::systemSettings() const
{
    return systemContext()->systemSettings();
}

} // namespace nx::vms::common
