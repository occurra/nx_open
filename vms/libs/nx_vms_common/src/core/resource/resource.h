// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <nx/utils/thread/mutex.h>
#include <nx/utils/uuid.h>

#include "resource_fwd.h"

/**
 * Base class for identifiable, observable objects in the VMS (servers, cameras, storages).
 *
 * Provides: id, name, url, parent-child relation, flags, status, and change signals.
 * Managed via shared pointers (QnResourcePtr).
 *
 * Key descendants:
 * - QnStorageResource - recording storage.
 *
 * Thread-safe.
 */
class NX_VMS_COMMON_API QnResource: public QObject
{
    Q_OBJECT

public:
    //---------------------------------------------------------------------------------------------
    // Constructing, copying and updating.

    /**
     * Creates a Resource belonging to some Common Module. The link is required mostly to access
     * various properties stored outside of the Resource.
     */
    QnResource();

    // Resource copying is prohibited as it creates an inconsistent entity: duplicate Resource,
    // which can contain link to Resource pool but does not belong to it.
    QnResource(const QnResource&) = delete;

    virtual ~QnResource() override;

    //---------------------------------------------------------------------------------------------
    // Data fields.

    /**
     * Primary resource identifier. Must never be changed after the Resource is used, so it is not
     * guarded by a mutex.
     */
    const nx::Uuid& getId() const { return m_id; }

    /**
     * Updates the Resource id. The function is not protected by the mutex, as the existing
     * Resource id must never be changed after the Resource is added to a Resource Pool.
     */
    void setIdUnsafe(const nx::Uuid& id);

    /**
     * Resource url. Has a different meaning depending on the Resource type: network url for
     * Devices, Servers and Web Pages, filesystem path for Storages and local files.
     */
    virtual QString getUrl() const = 0;

protected:
    /** Recursive mutex that is used when accessing Resource fields. */
    mutable nx::Mutex m_mutex;

private:
    /** Identifier of this Resource. */
    nx::Uuid m_id;
};
