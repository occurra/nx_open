// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <QtCore/QList>

#include <nx/media/media_data_packet.h>
#include <nx/media/metadata_type.h>
#include <nx/media/stream_event.h>

struct QnAbstractCompressedMetadata;
struct QnCompressedMetadata;

using QnAbstractCompressedMetadataPtr = std::shared_ptr<QnAbstractCompressedMetadata>;
using QnConstAbstractCompressedMetadataPtr = std::shared_ptr<const QnAbstractCompressedMetadata>;
using QnCompressedMetadataPtr = std::shared_ptr<QnCompressedMetadata>;
using QnConstCompressedMetadataPtr = std::shared_ptr<const QnCompressedMetadata>;

/** True when packet is overlay metadata; see isOverlayMetadata(MetadataType). */
NX_MEDIA_CORE_API bool isOverlayMetadata(const QnConstAbstractMediaDataPtr& packet);

struct NX_MEDIA_CORE_API QnAbstractCompressedMetadata: public QnAbstractMediaData
{
public:
    QnAbstractCompressedMetadata(MetadataType type, size_t bufferSize);

    virtual bool containTime(const qint64 timeUsec) const;

    qint64 duration() const;

    virtual void setData(nx::utils::ByteArray&& buffer) override;

public:
    MetadataType metadataType;
    qint64 m_duration = 0;
    nx::utils::ByteArray m_data;
};

struct NX_MEDIA_CORE_API QnCompressedMetadata: public QnAbstractCompressedMetadata
{
    using base_type = QnAbstractCompressedMetadata;

    QnCompressedMetadata(MetadataType type, int bufferSize = 0);

    /**
     * Analytics categories of the packet's items, e.g. "object.person", "event.sceneInsight" -
     * one for a single-item packet, the distinct categories of a list.
     */
    std::vector<std::string> categories;

    /**
     * Every item is a still object detection. Such a packet is recorded like any other but must
     * not arm event-gated recording.
     */
    bool still = false;

    virtual QnAbstractMediaData* clone() const override;

    virtual const char* data() const override;
    virtual size_t dataSize() const override;

    void setData(const char* data, std::size_t dataSize);
    void setData(const QByteArray& data);
    using QnAbstractCompressedMetadata::setData;

    void setDurationUsec(qint64 value) { m_duration = value; }
    void setTimestampUsec(qint64 value) { timestamp = value; }

    static QnCompressedMetadataPtr createMediaEventPacket(
        qint64 timestampUs,
        nx::media::StreamEvent value,
        const QByteArray& extraData = QByteArray());

    nx::media::StreamEventPacket toMediaEventPacket() const;

protected:
    void assign(const QnAbstractCompressedMetadata* other);
};

using FrameMetadata = QList<QnAbstractCompressedMetadataPtr>;
