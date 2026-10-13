// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

extern "C" {
// For typedef struct AVIOContext.
#include <libavformat/avio.h>
};

#include <QtCore/QSharedPointer>

#include <core/resource/avi/avi_archive_metadata.h>
#include <nx/media/audio_data_packet.h>
#include <nx/media/video_data_packet.h>
#include <nx/streaming/abstract_archive_delegate.h>
#include <nx/utils/move_only_func.h>
#include <nx/utils/thread/mutex.h>

struct AVPacket;
struct AVCodecContext;
struct AVFormatContext;
struct AVStream;
class QnAviAudioLayout;
class QnCustomResourceVideoLayout;

/**
 * Implements QnAbstractArchiveDelegate interface for accessing media data in file on local
 * filesystem. File format is not required to be AVI, the name is historical.
 */
class NX_VMS_COMMON_API QnAviArchiveDelegate: public QnAbstractArchiveDelegate
{
    using base_type = QnAbstractArchiveDelegate;
    friend class QnAviAudioLayout;

public:
    QnAviArchiveDelegate();
    virtual ~QnAviArchiveDelegate() override;

    virtual bool open(const QString& url) override;

    bool opened() const;
    virtual void close() override;
    virtual qint64 startTime() const override;
    virtual void setStartTimeUs(qint64 startTimeUs);
    virtual qint64 endTime() const override;
    virtual QnAbstractMediaDataPtr getNextData() override;

    /**
     * Move current position in the archive according to time and findIFrame provided.
     * @param time Desired time to set current position to, microseconds. Actual position will
     *     be set to time near desired time (i.e. not exactly), depending on findIFrame.
     * @param findIFrame If true, current position is set to key frame just before time specified.
     *     If false, behavior is not strictly defined. In reality, current position is set to key
     *     frame just after time desired, but this is not specified, and is not tested to work
     *     in all cases. Some using classes assume that if findIFrame is false, current position
     *     will be set exactly to frame nearest to time specified; that is mistake in fact.
     * @return An echo of time argument on success, -1 otherwise.
     */
    virtual qint64 seek(qint64 time, bool findIFrame) override;

    /**
     * In case of ffmpeg seek failure this function will try to reopen the file. If the reopening
     * has been successful the original timeToSeek will be returned but the file will be read from
     * the beginning.
     */
    qint64 seekWithFallback(qint64 time, bool findIFrame);

    /**
     * Move the current position to an exact byte offset in the underlying
     * file using AVSEEK_FLAG_BYTE. The offset must point at (or just before)
     * a TS-packet boundary; libavformat will resync forward to the next valid
     * TS sync byte if the offset is slightly off. Intended for callers that
     * own an out-of-band keyframe index for MPEG-TS files where av_seek_frame
     * with AVSEEK_FLAG_BACKWARD does not reliably land on a key frame.
     * @return 0 on success, kSeekError on failure.
     */
    qint64 seekToByteOffset(qint64 byteOffset);

    /**
     * Declares where the file's media timestamps begin, instead of leaving it to
     * avformat_find_stream_info to discover. Emitted timestamps are anchored at
     * startTimeUs by subtracting this, so a caller that wrote the file - and
     * therefore knows the value - can skip the discovery entirely. That matters
     * for two reasons: fast-open mode aborts the analysis before it can determine
     * a stream's start_time, and a reader fetching byte ranges from the middle of
     * a file never sees the start to discover it from.
     *
     * Call after open(), which is when the stream time bases needed for the
     * conversion are known; a value declared before open() is silently lost. Once
     * set, the value sticks for the delegate's lifetime: it survives the internal
     * reopen() that seek() performs on a file without video and that
     * seekWithFallback() performs after a faulty seek - both re-run findStreams(),
     * which would otherwise go back to guessing from start_time.
     * @param firstDtsUs Microseconds from the file's zero to its first
     *     primary-stream packet.
     */
    void setFirstDtsUs(qint64 firstDtsUs);

    void setForcedVideoChannelsCount(int videoChannels);
    virtual QnConstResourceVideoLayoutPtr getVideoLayout() override;
    virtual AudioLayoutConstPtr getAudioLayout() override;
    virtual bool hasVideo() const override;

    virtual bool setAudioChannel(unsigned num) override;

    // for optimization
    // void doNotFindStreamInfo();
    void setFastStreamFind(bool value);
    bool isStreamsFound() const;
    void setUseAbsolutePos(bool value);
    void setStorage(const QnStorageResourcePtr& storage);
    virtual bool providesMotionPackets() const override;

    const char* getTagValue(const char* tagName);

    virtual std::optional<QnAviArchiveMetadata> metadata() const override;

    QIODevice* ioDevice() const;
    bool readFailed() const;

    using BeforeOpenInputCallback = std::function<void(QnAviArchiveDelegate*)>;
    void setBeforeOpenInputCallback(BeforeOpenInputCallback callback);

    virtual bool reopen() override;

private:
    qint64 packetTimestamp(const AVPacket& packet);
    void packetTimestamp(QnCompressedVideoData* video, const AVPacket& packet);
    void initLayoutStreams();
    bool initMetadata();
    void fillVideoLayout();

    bool findStreams();
    CodecParametersConstPtr getCodecContext(AVStream* stream);
    /**
     * Recovers sample_rate / ch_layout / sample format on an audio stream whose
     * codecpar came back empty (fast-open mode cuts avformat_find_stream_info
     * short before it parses audio). Decodes packet once and writes the learned
     * params back into stream->codecpar. No-op when the params are already set.
     */
    void fillAudioParamsFromPacket(AVStream* stream, const AVPacket* packet);

    virtual bool checkStorage();

protected:
    QString m_url;
    unsigned m_selectedAudioChannel = 0;
    QnStorageResourcePtr m_storage;

private:
    AVFormatContext* m_formatContext = nullptr;
    AVIOContext* m_IOContext = nullptr;

    int m_audioStreamIndex = -1;
    int m_firstVideoIndex = 0;
    bool m_streamsFound = false;
    QnCustomResourceVideoLayoutPtr m_videoLayout;
    AudioLayoutPtr m_audioLayout;
    std::map<int, int> m_indexToChannel;
    QList<CodecParametersConstPtr> m_contexts;
    QnAviArchiveMetadata m_metadata;

    qint64 m_startTimeUs = 0;
    bool m_useAbsolutePos = true;
    qint64 m_durationUs = AV_NOPTS_VALUE;

    bool m_eofReached = false;
    nx::Mutex m_openMutex;
    QList<qint64> m_lastPacketTimes;
    bool m_fastStreamFind = false;
    bool m_hasVideo = true;
    qint64 m_lastSeekTime = AV_NOPTS_VALUE;
    qint64 m_firstDts = 0; //< DTS of the very first video packet
    /// Set when setFirstDtsUs has declared the base, which makes findStreams() stop
    /// deriving m_firstDts from start_time - it runs again on every reopen() and
    /// would otherwise discard the declaration.
    qint64 m_declaredFirstDtsUs = AV_NOPTS_VALUE;
    /**
     * Designates if a key frame has been found for a channel, that is, if
     * m_keyFrameFound[0] == true, than we have already found a key frame for the channel 0,
     * otherwise, we haven't.
     */
    std::vector<bool> m_keyFrameFound;
    double m_speed = 1.0;
    BeforeOpenInputCallback m_beforeOpenInputCallback = nullptr;
    bool m_readFailed = false;
    std::optional<int> m_forceVideoChannelsCount;
};

typedef QSharedPointer<QnAviArchiveDelegate> QnAviArchiveDelegatePtr;
