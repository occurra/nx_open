// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <core/resource/resource_media_layout.h>
#include <nx/media/abstract_data_packet.h>
#include <nx/utils/lockable.h>

class QnAbstractMediaDataReceptor;

#define CL_MAX_CHANNEL_NUMBER (10)

class NX_VMS_COMMON_API QnAbstractStreamDataProvider
{
public:
    QnAbstractStreamDataProvider();
    virtual ~QnAbstractStreamDataProvider();

    virtual bool dataCanBeAccepted() const;

    int processorsCount() const;
    virtual void addDataProcessor(QnAbstractMediaDataReceptor* dp);
    virtual void removeDataProcessor(QnAbstractMediaDataReceptor* dp);

    virtual QnConstResourceVideoLayoutPtr getVideoLayout() const
    {
        return QnConstResourceVideoLayoutPtr();
    }
    virtual bool hasVideo() const { return true; }

protected:
    virtual void putData(const QnAbstractDataPacketPtr& data);

    using LockedDataProcessors = nx::Lockable<QList<QnAbstractMediaDataReceptor*>>::LockedType;

    // Register / unregister a processor when the m_dataprocessors lock is already
    // held by the caller, so a subclass can extend add/remove atomically with its
    // own per-processor state. Pass the handle returned by m_dataprocessors.lock().
    void addDataProcessorUnsafe(
        LockedDataProcessors& dataProcessors,
        QnAbstractMediaDataReceptor* dp);
    void removeDataProcessorUnsafe(
        LockedDataProcessors& dataProcessors,
        QnAbstractMediaDataReceptor* dp);

protected:
    nx::Lockable<QList<QnAbstractMediaDataReceptor*>> m_dataprocessors;
};

using QnAbstractStreamDataProviderPtr = std::shared_ptr<QnAbstractStreamDataProvider>;
