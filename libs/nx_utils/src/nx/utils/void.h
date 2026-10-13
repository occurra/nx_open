// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QString>

#include <nx/reflect/instrument.h>
#include <nx/reflect/json.h>

namespace nx::utils {

struct Void
{
    inline Void getId() const { return Void(); }
    inline QString toString() const { return QString(); }
    bool operator==(const Void&) const { return false; }
};

NX_REFLECTION_INSTRUMENT(Void, )

} // namespace nx::utils
