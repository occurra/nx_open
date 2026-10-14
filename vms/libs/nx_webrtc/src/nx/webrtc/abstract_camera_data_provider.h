// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <optional>

#include <core/dataconsumer/abstract_data_receptor.h>
#include <nx/media/annexb_to_mp4.h>
#include <nx/streaming/abstract_data_consumer.h>
#include <nx/streaming/archive_stream_reader.h>
#include <nx/utils/uuid.h>
#include <nx/vms/api/types/stream_index.h>
#include <transcoding/media_transcoder.h>

namespace nx::metric { struct Storage; }

namespace nx::webrtc {

class Consumer;

class NX_WEBRTC_API AbstractCameraDataProvider: public QnAbstractMediaDataReceptor
{
public:
    // Data handler, will put nullptr on EOF.
    using OnDataHandler = std::function<void(const QnConstAbstractMediaDataPtr& packet)>;

    int64_t getPosition() const { return m_positionUs; }
    void setPosition(int64_t positionUs) { m_positionUs = positionUs; }

    bool isInitialized() const { return m_initialized; }
    void setInitialized(bool value) { m_initialized = value; }
    virtual bool isStarted() const = 0;

    void setDataConsumer(std::weak_ptr<Consumer> consumer);

public: // Initialize.
    virtual bool addVideoTranscoder(
        QnFfmpegVideoTranscoder::Config config,
        const nx::core::transcoding::Settings& transcodingSettings) = 0;
    virtual bool addAudioTranscoder(AVCodecID codec) = 0;
    virtual bool isTranscodingEnabled(QnAbstractMediaData::DataType dataType) const = 0;

    virtual CodecParametersConstPtr getVideoCodecParameters() const = 0;
    virtual CodecParametersConstPtr getAudioCodecParameters() const = 0;

    /// Seed the cached video/audio codec parameters so the next encoder rebuild
    /// uses them instead of re-probing the source. Used on a mid-stream codec
    /// change: the packet that triggered it carries the authoritative new
    /// parameters, avoiding a racy archive re-probe that can read the wrong
    /// segment's codec.
    virtual void setVideoCodecParameters(const CodecParametersConstPtr& /*params*/) {}
    virtual void setAudioCodecParameters(const CodecParametersConstPtr& /*params*/) {}

    nx::Uuid deviceId() const { return m_deviceId; }
    void setDeviceId(const nx::Uuid& id) { m_deviceId = id; }

    virtual void setEnableMetadata(bool /*enable*/) {}

public: // Playback.
    virtual bool isLiveMode() const = 0;
    virtual bool paused() const = 0;
    /// Open (or reopen) the source at positionUs and start delivering. With
    /// keyframeOnly the archive lands at the nearest preceding key frame instead
    /// of pre-rolling to the exact position - the same rough/precise choice
    /// seek() takes, passed here because this is the path a seek falls through
    /// to whenever there is nothing open to reposition.
    virtual bool play(int64_t positionUs, bool useAudioLastGop, bool keyframeOnly = false) = 0;
    /// Reposition playback to positionUs. The default rebuilds the pipeline via
    /// play(); providers that can reposition an already-open source in place
    /// override this to preserve pause state and avoid the teardown. With
    /// keyframeOnly the seek lands at the nearest preceding key frame instead of
    /// pre-rolling to the exact position - a cheap preview for timeline scrubbing.
    /// marker, when set, is stamped onto the opaque field of packets delivered
    /// from this seek, so the muxing side can tell post-seek packets from the
    /// pre-seek flush. It is bound to the seek itself (not a separate call) so a
    /// source reading on another thread cannot pair it with a different seek.
    virtual bool seek(
        int64_t positionUs,
        bool keyframeOnly = false,
        std::optional<int> /*marker*/ = std::nullopt)
    {
        return play(positionUs, /*useAudioLastGop*/ false, keyframeOnly);
    }
    virtual bool changeQuality(nx::vms::api::StreamIndex stream) = 0;
    virtual bool pause() = 0;
    /// Resume paused playback. positionUs, when set, repositions the source there first -
    /// a stream paused on live resumes as archive exactly at the client's held frame.
    /// marker follows the same contract as seek()'s.
    virtual bool resume(
        std::optional<int64_t> positionUs = std::nullopt,
        std::optional<int> marker = std::nullopt) = 0;
    /// Deliver the next video frame while paused. positionUs, when set, seeks there first,
    /// so the delivered frame is the one just past that position.
    virtual bool nextFrame(std::optional<int64_t> positionUs = std::nullopt) = 0;
    /// Deliver the previous video frame while paused. positionUs, when set, seeks there
    /// first, so the shown frame is the one just before that position.
    virtual bool previousFrame(std::optional<int64_t> positionUs = std::nullopt) = 0;
    virtual void stop() = 0;

protected:
    nx::Uuid m_deviceId;
    int64_t m_positionUs = DATETIME_NOW;
    bool m_initialized = false;
    std::weak_ptr<Consumer> m_consumer;
};

using AbstractCameraDataProviderPtr = std::shared_ptr<AbstractCameraDataProvider>;
using createDataProviderFactory = nx::MoveOnlyFunc<AbstractCameraDataProviderPtr(
    nx::Uuid /*deviceId*/,
    std::optional<std::chrono::milliseconds> /*positionMs*/,
    nx::vms::api::StreamIndex /*stream*/,
    std::optional<float> /*speedOpt*/,
    std::string_view /*sessionId*/)>;

} // namespace nx::webrtc
