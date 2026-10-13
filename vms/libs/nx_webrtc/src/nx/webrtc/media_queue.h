// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <deque>
#include <optional>
#include <vector>

#include <nx/media/media_data_packet.h>
#include <nx/utils/thread/mutex.h>

namespace nx::vms::server {

class NX_WEBRTC_API MediaQueue
{
public:
    MediaQueue(int maxSize);
    void putData(const QnConstAbstractMediaDataPtr& data);
    /**
     * Marks video as already having a key frame - on every channel present and
     * on any that appears later. For a queue fed from the middle of a stream,
     * where the key frame has already passed and holding video back until the
     * next one would discard the rest of the GOP.
     */
    void setKeyDataFound(bool value);
    /// Re-insert an already-dequeued packet at the front so it is popped next.
    /// Used to retry the packet that triggered a codec switch, ahead of frames
    /// buffered while the renegotiation was in flight. Bypasses the keyframe
    /// gating and overflow trimming in putData() — the packet is already vetted.
    void pushFront(const QnConstAbstractMediaDataPtr& data);
    QnConstAbstractMediaDataPtr popData(bool blocked = false);
    void stop();
    void setMaxSize(int maxSize);
    /**
     * Drops the queued packets: every device's, or one device's when deviceId is given. A
     * seek flushes only the seeking device's backlog this way: the queue is shared by every
     * camera of a session, and their post-seek key frames arrive within a millisecond of each
     * other, so a session-wide flush on one camera's BOF discarded the others'. The key-frame
     * gate is per channel and shared the same way, so a per-device flush leaves it alone.
     */
    void clearUnprocessedData(std::optional<nx::Uuid> deviceId = std::nullopt);

    int size() const;

private:
    std::chrono::microseconds getDurationUnsafe();
    void clearTillLastGopUnsafe();

private:
    int m_maxSize;
    std::deque<QnConstAbstractMediaDataPtr> m_mediaQueue;
    std::vector<bool> m_keyDataFound;

    nx::WaitCondition m_waitForFrame;
    mutable nx::Mutex m_mutex;
    std::atomic<bool> m_needToStop = false;
};

} // namespace nx::vms::server
