// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "meta_data_packet.h"

#include <nx/media/config.h>

//--------------------------------- QnAbstractCompressedMetadata ---------------------------------

QnAbstractCompressedMetadata::QnAbstractCompressedMetadata(MetadataType type, size_t bufferSize):
    QnAbstractMediaData(METADATA),
    metadataType(type),
    m_duration(0),
    m_data(CL_MEDIA_ALIGNMENT, bufferSize, AV_INPUT_BUFFER_PADDING_SIZE)
{
}

bool QnAbstractCompressedMetadata::containTime(const qint64 timeUsec) const
{
    if (m_duration == 0)
        return timestamp == timeUsec;
    else if (timestamp <= timeUsec && timestamp + m_duration > timeUsec)
        return true;

    return false;
}

qint64 QnAbstractCompressedMetadata::duration() const
{
    return m_duration;
}

void QnAbstractCompressedMetadata::setData(nx::utils::ByteArray&& buffer)
{
    m_data = std::move(buffer);
}

//------------------------------------ QnCompressedMetadata --------------------------------------

QnCompressedMetadata::QnCompressedMetadata(MetadataType type, int bufferSize):
    QnAbstractCompressedMetadata(type, bufferSize)
{
}

QnAbstractMediaData* QnCompressedMetadata::clone() const
{
    auto cloned = new QnCompressedMetadata(metadataType, 0);
    cloned->assign(this);
    return cloned;
}

void QnCompressedMetadata::assign(const QnAbstractCompressedMetadata* other)
{
    base_type::assign(other);
    m_data = other->m_data;
    if (const auto* cm = dynamic_cast<const QnCompressedMetadata*>(other))
    {
        categories = cm->categories;
        still = cm->still;
    }
}

const char* QnCompressedMetadata::data() const
{
    return m_data.constData();
}

size_t QnCompressedMetadata::dataSize() const
{
    return m_data.size();
}

void QnCompressedMetadata::setData(const char* data, std::size_t dataSize)
{
    m_data.clear();
    m_data.write(data, dataSize);
}

void QnCompressedMetadata::setData(const QByteArray& data)
{
    setData(data.data(), data.size());
}

QnCompressedMetadataPtr QnCompressedMetadata::createMediaEventPacket(
    qint64 timestampUs,
    nx::media::StreamEvent value,
    const QByteArray& extraData)
{
    QnCompressedMetadataPtr rez(new QnCompressedMetadata(MetadataType::mediaStreamEvent));
    rez->timestamp = timestampUs;

    nx::media::StreamEventPacket packet;
    packet.code = value;
    packet.extraData = extraData;
    rez->setData(nx::media::serialize(packet));
    return rez;
}

nx::media::StreamEventPacket QnCompressedMetadata::toMediaEventPacket() const
{
    const QByteArray data = QByteArray::fromRawData(m_data.data(), (int) m_data.size());
    nx::media::StreamEventPacket result;
    nx::media::deserialize(data, &result);
    return result;
}

bool isOverlayMetadata(const QnConstAbstractMediaDataPtr& packet)
{
    if (!packet || packet->dataType != QnAbstractMediaData::METADATA)
        return false;

    const auto meta = std::dynamic_pointer_cast<const QnAbstractCompressedMetadata>(packet);
    return meta && isOverlayMetadata(meta->metadataType);
}
