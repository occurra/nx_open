// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QString>

namespace nx::branding {

/** Company name. */
NX_BRANDING_API QString company();

/** Internal company id. */
NX_BRANDING_API QString companyId();

/** Default locale code for all ui applications. */
NX_BRANDING_API QString defaultLocale();

} // namespace nx::branding
