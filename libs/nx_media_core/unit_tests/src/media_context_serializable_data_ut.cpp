// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include <gtest/gtest.h>

#include <nx/media/av_codec_helper.h>
#include <nx/media/media_context_serializable_data.h>

namespace nx::streaming::test {

TEST(MediaContextSerializableData, main)
{
    QnMediaContextSerializableData data;
    data.codecId = AV_CODEC_ID_H264;
    data.codecType = AVMEDIA_TYPE_VIDEO;
    data.sampleFmt = AV_SAMPLE_FMT_U8;
    data.bitsPerCodedSample = 8;
    data.width = 1920;
    data.height = 1080;
    data.bitRate = 80000000;
    data.channelLayout = 42;
    data.blockAlign = 32;

    const QByteArray serializedData = data.serialize();
    ASSERT_FALSE(serializedData.isEmpty());

    QnMediaContextSerializableData deserialized;
    ASSERT_TRUE(deserialized.deserialize(serializedData));

    ASSERT_EQ(data.codecId, deserialized.codecId);
    ASSERT_EQ(data.codecType, deserialized.codecType);
    ASSERT_EQ(data.sampleFmt, deserialized.sampleFmt);
    ASSERT_EQ(data.bitsPerCodedSample, deserialized.bitsPerCodedSample);
    ASSERT_EQ(data.width, deserialized.width);
    ASSERT_EQ(data.height, deserialized.height);
    ASSERT_EQ(data.bitRate, deserialized.bitRate);
    ASSERT_EQ(data.channelLayout, deserialized.channelLayout);
    ASSERT_EQ(data.blockAlign, deserialized.blockAlign);
}

} // namespace nx::streaming::test
