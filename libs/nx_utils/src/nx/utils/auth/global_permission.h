// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QFlags>

#include <nx/reflect/enum_instrument.h>

namespace nx::utils::auth {

/**%apidoc
 * Flags describing global User capabilities, independently of Resources.
 * %// Stored in the database.
 */
enum class GlobalPermission
{
    /**%apidoc
     * No global permissions.
     */
    none = 0,

    /* Administrative permissions. */

    /**%apidoc
     * Administrator permissions: full control of the VMS Site.
     */
    administrator = 1 << 0,
};
NX_REFLECTION_INSTRUMENT_ENUM(GlobalPermission, none, administrator)

Q_DECLARE_FLAGS(GlobalPermissions, GlobalPermission)
Q_DECLARE_OPERATORS_FOR_FLAGS(GlobalPermissions)

} // namespace nx::utils::auth
