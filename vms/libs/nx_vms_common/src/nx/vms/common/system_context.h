// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QObject>

#include <nx/utils/impl_ptr.h>
#include <nx/utils/log/assert.h>
#include <nx/utils/uuid.h>

#include "system_context_aware.h" //< Forward declarations.

namespace nx::metric { struct Storage; }

namespace nx::vms::common {

class SystemSettings;

/**
 * Storage for the application Resource Context classes. One Resource Context corresponds to one
 * client-server connection. Also there can be separate Resource Context for the local files in the
 * Desktop Client. VMS Server has the only one Resource Context for it's own database connection.
 */
class NX_VMS_COMMON_API SystemContext: public QObject
{
    Q_OBJECT

public:
    /**
     * @param peerId Id of the current peer in the Message Bus. It is persistent and is not changed
     *     between the application runs. See ::peerId() for the details.
     */
    SystemContext(nx::Uuid peerId, QObject* parent = nullptr);
    virtual ~SystemContext();

    /**
     * Id of the current peer in the Message Bus. It is persistent and is not changed between the
     * application runs. It is stored in the application settings. VMS Server uses it as a Server
     * Resource id. Desktop Client calculates actual peer id depending on the stored persistent id
     * and on the number of the running client instance, so different Client windows have different
     * peer ids.
     */
    const nx::Uuid& peerId() const;

    /**
     * System settings, which do not depend on any Device or Server and are applied globally.
     */
    SystemSettings* systemSettings() const;

    std::shared_ptr<nx::metric::Storage> metrics() const;

private:
    struct Private;
    nx::utils::ImplPtr<Private> d;
};

} // namespace nx::vms::common
