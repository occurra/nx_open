// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <atomic>
#include <functional>

#include <nx/network/aio/basic_pollable.h>

#include "webrtc_ice.h"

namespace nx::webrtc {

class Session;

/**
 * Callbacks a DataPeer owner installs before signaling starts. All of them are
 * invoked on the session's AIO thread.
 */
struct DataPeerHandlers
{
    /// The datachannel completed DCEP: the pipe is usable in both directions.
    std::function<void()> onOpen;

    /// One complete binary message from the peer (reassembled across SCTP
    /// fragments by the DataChannel layer).
    std::function<void(std::string data)> onMessage;

    /// The transport failed (ICE/DTLS error, socket error, peer-silence
    /// reap). Fired at most once; no other handler fires after it. NOT fired
    /// when the owner itself drops the session - the owner already knows.
    std::function<void()> onClosed;

    /// The datachannel output queue shrank to queueDepth messages. Producers
    /// gating on sendQueueDepth() use it to resume a parked backlog.
    std::function<void(int queueDepth)> onQueueProgress;
};

/**
 * Transceiver for Purpose::data sessions: a datachannel-only endpoint carrying
 * opaque binary messages, with no media tracks, consumer or providers. Both
 * signaling roles use it - the offerer opens the DCEP channel, the answerer
 * accepts it - and the owner talks to the peer solely through sendBinary() and
 * the installed DataPeerHandlers.
 */
class NX_WEBRTC_API DataPeer: public AbstractIceDelegate
{
public:
    /// @param aioThread The session's AIO thread (Session::aioThread()). Every
    ///     outbound message hops onto it, so sends serialize with the ices and
    ///     with the teardown that runs there - that ordering is what makes
    ///     sendBinary() safe to call from any thread.
    DataPeer(Session* session, nx::network::aio::AbstractAioThread* aioThread);
    virtual ~DataPeer() override;

    /// Install the owner's callbacks. Must be called before the session's
    /// tracker starts: the handlers are read from the session thread with no
    /// synchronization once signaling is under way.
    void setHandlers(DataPeerHandlers handlers);

    // From AbstractIceDelegate. Called on the session's AIO thread.
    virtual bool start(Ice* ice, Dtls* dtls) override;
    virtual void stop() override;
    virtual bool onSrtp(std::vector<uint8_t> buffer) override;
    virtual void onDataChannelString(const std::string& data, int streamId) override;
    virtual void onDataChannelBinary(const std::string& data, int streamId) override;
    virtual void onDataChannelOpened(int streamId) override;
    virtual void onDataChannelQueueProgress(int queueDepth) override;

    /**
     * Queue one binary message to the peer. Thread-safe. Messages sent before
     * the channel opens are buffered by the DataChannel layer (bounded); a
     * message enqueued after the transport closed is silently dropped.
     * @return False when the transport is closed or not yet started.
     */
    bool sendBinary(std::string data);

    /**
     * Depth of the datachannel output queue, in ~16 KiB fragments.
     * Thread-safe. Producers use it as the congestion signal: park above a
     * high watermark, resume from onQueueProgress below a low one.
     */
    int sendQueueDepth() const;

    /// Whether DCEP completed and the transport has not closed since.
    bool isOpen() const;

private:
    Session* m_session = nullptr;
    DataPeerHandlers m_handlers;
    nx::network::aio::BasicPollable m_pollable;
    std::atomic<Ice*> m_ice{nullptr};
    std::atomic<bool> m_open{false};
    std::atomic<bool> m_closed{false};
};

} // namespace nx::webrtc
