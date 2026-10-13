// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QPointer>

#include <nx/utils/impl_ptr.h>
#include <nx/utils/uuid.h>

namespace nx::vms::common {

class SystemContext;
class SystemSettings;

/**
 * Helper class for the SystemContext-dependent classes. Must be destroyed before the Context is.
 */
class NX_VMS_COMMON_API SystemContextAware
{
public:
    SystemContextAware(SystemContext* context);

    /**
     * Virtual destructor.
     * We do dynamic_casts to SystemContextAware, so this class must have a vtable.
     */
    virtual ~SystemContextAware();

    /**
     * Linked context. May never be changed. Must exist when SystemContextAware is destroyed.
     */
    SystemContext* systemContext() const;

    nx::Uuid peerId() const;

    SystemSettings* systemSettings() const;

private:
    struct Private;
    nx::utils::ImplPtr<Private> d;
};

} // namespace nx::vms::common
