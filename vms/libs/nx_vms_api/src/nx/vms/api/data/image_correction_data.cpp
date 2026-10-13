// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "image_correction_data.h"

#include <QtCore/QList>

#include <nx/reflect/compare.h>

namespace nx::vms::api {

QByteArray ImageCorrectionData::toByteArray() const
{
    return QByteArray::number(blackLevel)
        .append(';')
        .append(QByteArray::number(whiteLevel))
        .append(';')
        .append(QByteArray::number(gamma))
        .append(';')
        .append(enabled ? '1' : '0')
        .append((char) 0);
}

ImageCorrectionData ImageCorrectionData::fromByteArray(const QByteArray& data)
{
    ImageCorrectionData result;
    QList<QByteArray> params = data.split(';');
    if (params.size() >= 4)
    {
        result.blackLevel = params[0].toDouble();
        result.whiteLevel = params[1].toDouble();
        result.gamma = params[2].toDouble();
        result.enabled = params[3].toInt();
    }
    return result;
}

bool ImageCorrectionData::operator==(const ImageCorrectionData& other) const
{
    return nx::reflect::equals(*this, other);
}

} // namespace nx::vms::api
