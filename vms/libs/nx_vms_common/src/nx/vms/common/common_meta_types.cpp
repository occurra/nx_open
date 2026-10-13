// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "common_meta_types.h"

#include <mutex>

#include <QtNetwork/QAuthenticator>
#include <QtNetwork/QHostAddress>

#include <core/resource/resource.h>
#include <core/resource/resource_fwd.h>
#include <core/resource/storage_resource.h>
#include <nx/media/abstract_data_packet.h>
#include <nx/media/media_data_packet.h>
#include <nx/media/meta_data_packet.h>
#include <nx/network/socket_common.h>
#include <nx/streaming/archive_stream_reader.h>
#include <nx/string.h>
#include <nx/utils/metatypes.h>
#include <nx/utils/uuid.h>
#include <nx/vms/api/metatypes.h>
#include <recording/time_period.h>
#include <recording/time_period_list.h>

namespace nx::vms::common {

void initializeMetatypesInternal()
{
    nx::utils::Metatypes::initialize();
    nx::vms::api::Metatypes::initialize();

    qRegisterMetaType<uintptr_t>("uintptr_t");

    qRegisterMetaType<QHostAddress>();
    qRegisterMetaType<QAuthenticator>();
    qRegisterMetaType<Qt::ConnectionType>();

    qRegisterMetaType<nx::network::HostAddress>();
    qRegisterMetaType<nx::network::SocketAddress>();

    qRegisterMetaType<QnResourcePtr>();

    qRegisterMetaType<QnStorageResourcePtr>();

    qRegisterMetaType<QnAbstractDataPacketPtr>();
    qRegisterMetaType<QnConstAbstractDataPacketPtr>();

    // Required for queuing arguments of type 'QnArchiveStreamReader*' in QObject::connect.
    qRegisterMetaType<QnArchiveStreamReader>();

    qRegisterMetaType<QList<QMap<QString, QString>>>();

    qRegisterMetaType<QnAbstractCompressedMetadataPtr>();

    qRegisterMetaType<nx::String>();
}

void initializeMetaTypes()
{
    static std::once_flag initialized;
    std::call_once(initialized, &initializeMetatypesInternal);
}

} // namespace nx::vms::common
