// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "abstract_stream_data_provider.h"

#include <core/dataconsumer/abstract_data_receptor.h>
#include <core/resource/resource.h>
#include <nx/utils/log/log.h>

QnAbstractStreamDataProvider::QnAbstractStreamDataProvider()
{
}

QnAbstractStreamDataProvider::~QnAbstractStreamDataProvider()
{
}

bool QnAbstractStreamDataProvider::dataCanBeAccepted() const
{
    // need to read only if all queues has more space and at least one queue is exist
    auto dataProcessors = m_dataprocessors.lock();
    for (int i = 0; i < dataProcessors->size(); ++i)
    {
        QnAbstractMediaDataReceptor* dp = dataProcessors->at(i);
        if (!dp->canAcceptData())
            return false;
    }

    return true;
}

int QnAbstractStreamDataProvider::processorsCount() const
{
    return m_dataprocessors.lock()->size();
}

void QnAbstractStreamDataProvider::addDataProcessor(QnAbstractMediaDataReceptor* dp)
{
    NX_CRITICAL(dp);
    auto dataProcessors = m_dataprocessors.lock();
    if (!dataProcessors->contains(dp))
        addDataProcessorUnsafe(dataProcessors, dp);
}

void QnAbstractStreamDataProvider::addDataProcessorUnsafe(
    LockedDataProcessors& dataProcessors,
    QnAbstractMediaDataReceptor* dp)
{
    NX_DEBUG(this, "Add data processor: %1", dp);
    dataProcessors->push_back(dp);
    dp->consumers += 1;
}

void QnAbstractStreamDataProvider::removeDataProcessor(QnAbstractMediaDataReceptor* dp)
{
    if (!dp)
        return;

    auto dataProcessors = m_dataprocessors.lock();
    removeDataProcessorUnsafe(dataProcessors, dp);
}

void QnAbstractStreamDataProvider::removeDataProcessorUnsafe(
    LockedDataProcessors& dataProcessors,
    QnAbstractMediaDataReceptor* dp)
{
    if (dataProcessors->removeOne(dp))
    {
        dp->consumers -= 1;
        NX_DEBUG(this, "Remove data processor: %1", dp);
    }
    else
    {
        NX_DEBUG(this, "Remove not added data processor: %1", dp);
    }
}

void QnAbstractStreamDataProvider::putData(const QnAbstractDataPacketPtr& data)
{
    if (!data)
        return;

    const auto dataProcessors = m_dataprocessors.lock();
    for (const auto& p: *dataProcessors)
        p->putData(data);
}
