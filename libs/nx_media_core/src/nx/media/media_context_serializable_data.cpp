// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "media_context_serializable_data.h"

#include <nx/media/av_codec_helper.h>
#include <nx/reflect/json.h>
#include <nx/utils/log/assert.h>

QByteArray QnMediaContextSerializableData::serialize() const
{
    return QByteArray::fromStdString(nx::reflect::json::serialize(*this));
}

bool QnMediaContextSerializableData::deserialize(const QByteArray& data)
{
    return nx::reflect::json::deserialize(std::string_view(data.constData(), data.size()), this)
        .success;
}

void QnMediaContextSerializableData::initializeFrom(const AVCodecParameters* codecParams)
{
    codecId = (int) codecParams->codec_id;
    codecType = (int) codecParams->codec_type;
    if (codecParams->extradata)
    {
        extradata = QByteArray((const char*) codecParams->extradata, codecParams->extradata_size);
    }
    // TODO: support custom channel order.
    NX_ASSERT(
        codecParams->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC
        || codecParams->ch_layout.order == AV_CHANNEL_ORDER_NATIVE);
    channels = codecParams->ch_layout.nb_channels;
    channelLayout = codecParams->ch_layout.u.mask;
    sampleRate = codecParams->sample_rate;
    sampleFmt = (int) codecParams->format;
    bitsPerCodedSample = codecParams->bits_per_coded_sample;
    width = codecParams->width;
    height = codecParams->height;
    bitRate = codecParams->bit_rate;
    blockAlign = codecParams->block_align;
}
