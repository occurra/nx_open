// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <deque>

#include <nx/network/aio/basic_pollable.h>
#include <nx/network/aio/timer.h>
#include <nx/network/stun/message_parser.h>
#include <nx/network/stun/message_serializer.h>

#include "webrtc_datachannel.h"
#include "webrtc_dtls.h"
#include "webrtc_utils.h"

namespace nx::webrtc {

constexpr size_t kIceTcpSendBufferSize = 1024 * 64;

/**
 * Builds a serialized STUN binding *request* for a full-ICE connectivity check
 * sent by the media_server (controlled role) toward the browser.
 *
 * MESSAGE-INTEGRITY is keyed with @p remotePwd (the browser's ice-pwd) so the
 * browser can verify it — a request keyed with the server's own pwd is silently
 * rejected by the peer. USERNAME is "<remoteUfrag>:<localUfrag>". The request
 * also carries PRIORITY and ICE-CONTROLLED with the agent's 8-byte tie-breaker
 * (network byte order) for role-conflict resolution, plus a FINGERPRINT.
 */
NX_WEBRTC_API nx::Buffer buildControlledBindingRequest(
    const std::string& remoteUfrag,
    const std::string& localUfrag,
    const std::string& remotePwd,
    int priority,
    std::uint64_t tieBreaker);

class Ice;

class NX_WEBRTC_API AbstractIceDelegate
{
public:
    // All these methods are called from Ice's AIO Thread.
    virtual bool start(Ice* ice, Dtls* dtls) = 0;
    virtual void stop() = 0;
    virtual bool onSrtp(std::vector<uint8_t> buffer) = 0;
    virtual void onDataChannelString(const std::string& data, int streamId) = 0;
    virtual void onDataChannelBinary(const std::string& data, int streamId) = 0;

    /// The datachannel completed DCEP and is usable. Default no-op: the
    /// browser path never needed it; DataPeer keys its connected state on it.
    virtual void onDataChannelOpened(int /*streamId*/) {}

    /// The datachannel output queue shrank to queueDepth messages. Default
    /// no-op: the Consumer pump is gated separately inside Ice; DataPeer uses
    /// it to drain its producer-side backlog.
    virtual void onDataChannelQueueProgress(int /*queueDepth*/) {}

    virtual ~AbstractIceDelegate() = default;
};

class Ice: public AbstractDataChannelDelegate, public AbstractDtlsDelegate
{
protected:
    enum Stage
    {
        binding,
        dtls,
        streaming,
        unknown
    };

    /// Outcome of processing an incoming packet, returned up to onBytesRead().
    enum class PacketResult
    {
        fatal, ///< Unrecoverable (DTLS/SRTP error): tear the ice down.
        drop, ///< Foreign/unauthenticatable packet: ignore it, keep the ice.
        valid, ///< Authenticated check or own-protocol data: refresh the consent timer.
    };

public:
    /// @param aioThread Thread to run on, normally the owning session's (see
    ///     Session::aioThread()) so this ice, the Consumer and the session's
    ///     other ices all serialize on one thread. Null leaves the pool's
    ///     choice, which is all IceTcp can do: it is built from an accepted
    ///     connection, before the STUN username says which session it serves.
    Ice(SessionPool* sessionPool,
        const SessionConfig& config,
        aio::AbstractAioThread* aioThread = nullptr);
    virtual ~Ice();

    // Main API.
    void writePacket(const char* data, int size, bool foreground);

    // API for DataChannel.
    virtual void writeDataChannelPacket(const uint8_t* data, int size) override final;
    virtual void onDataChannelString(const std::string& data, int streamId) override final;
    virtual void onDataChannelBinary(const std::string& data, int streamId) override final;
    virtual void onDataChannelOpened(int streamId) override final;
    virtual void onDataChannelQueueProgress() override final;

    // API for Transceiver.
    void writeBatch(std::deque<nx::Buffer> mediaBuffers);
    aio::AbstractAioThread* getAioThread();
    /**
     * deviceId tags the message for the per-device flush on a seek (see
     * clearDataChannelStrings); null leaves it untagged. unreliableTransport sends
     * unordered and without retransmissions - see
     * DataChannel::Message::unreliableTransport.
     */
    void writeDataChannelString(
        const std::string& data,
        const nx::Uuid& deviceId = {},
        bool unreliableTransport = false);
    /** As writeDataChannelString, for a binary message. */
    void writeDataChannelBinary(
        nx::Buffer data,
        const nx::Uuid& deviceId = {},
        bool unreliableTransport = false);
    /**
     * Drops the queued-but-unsent data-channel strings of deviceId. Called on
     * the first post-seek packet so the device's pre-seek overlay metadata
     * backlog does not go out ahead of the post-seek stream. See
     * DataChannel::removeQueuedStrings().
     */
    void clearDataChannelStrings(const nx::Uuid& deviceId);
    int dataChannelQueueSize() const { return m_dataChannel.outputQueueSize(); }
    /**
     * Whether the data channel backlog is deep enough to pause the media pump; a shallow one is
     * not congestion. onDataChannelQueueProgress() resumes the pump once it shrinks.
     */
    bool isDataChannelBacklogDeep() const;

    virtual IceCandidate::Filter type() const = 0;
    virtual nx::network::SocketAddress iceRemoteAddress() const = 0;
    virtual nx::network::SocketAddress iceLocalAddress() const = 0;

    // API for Dtls.
    virtual void writeDtlsPacket(const char* data, int size) override final;
    virtual bool onDtlsData(const uint8_t* data, int size) override final;
    int bufferEmpty() const;

    /**
     * Stops this ice: cancels its IO, tears down the datachannel, stops the consumer chain and
     * unlocks and forgets the session. Idempotent in the subclasses (m_needStop). Callable any
     * time from this ice's own AIO thread - by the ice itself on a fatal packet or timeout, and
     * by the teardown task Session::stop() posts.
     */
    virtual void stopUnsafe();

protected:
    friend class Consumer;

    // API for subclasses.
    virtual nx::Buffer toSendBuffer(const char* data, int dataSize) const;
    virtual void asyncSendPacketUnsafe() = 0;

    /// Called by handleStun() once an incoming STUN message has passed the
    /// integrity check, with its authenticated source address. Default no-op;
    /// IceUdp overrides it to learn a peer-reflexive address and retarget its
    /// responses and connectivity checks toward the peer's real source.
    virtual void onPeerValidated(const nx::network::SocketAddress& /*peer*/) {}

    void onBytesWritten(SystemError::ErrorCode errorCode, std::size_t bytesTransferred);
    void sendNextBufferUnsafe();

    /** Queues buffer ahead of media, behind the other foreground buffers. Does not start the send.
     */
    void insertForegroundBuffer(nx::Buffer buffer);
    PacketResult processIncomingPacket(const uint8_t* data, int size);

    /// Whether a buffer is posted or being written right now. Raised before
    /// the send is posted and lowered only once it has completed, so it also
    /// covers the window where the send lambda sits in the socket's queue -
    /// which is what makes it the safe-to-migrate test for IceTcp.
    bool hasBufferToSend() const;

private:
    // Protocol related.
    Stage demuxMessageStage(const uint8_t* data, size_t size);
    PacketResult handleStun(const uint8_t* data, int size);
    PacketResult handleDtls(const uint8_t* data, int size);
    PacketResult handleSrtp(const uint8_t* data, int size);
    void sendStunError(const stun::Message& message);

protected:
    /// The send queue and the two offsets below are touched only on m_pollable's aio thread:
    /// the session binds this ice, its Consumer and its socket to one thread, every producer
    /// dispatches onto it, and IceTcp migrates both socket and pollable together while the
    /// queue is empty. That is why they need no lock - the entry points assert the thread
    /// instead, so a producer that ever appears from elsewhere is reported rather than
    /// silently racing.
    nx::network::aio::BasicPollable m_pollable;
    bool m_needStop = false;

    /// Set by handleStun() when the STUN username names a session whose aio
    /// thread is not ours - only IceTcp, which the tcp listener builds before
    /// anyone knows which session it serves. It then hands itself over at the
    /// next safe point; see IceTcp::processBufferedPackets(). Null for the
    /// ices the session creates itself: those are born on its thread.
    aio::AbstractAioThread* m_pendingAioThread = nullptr;

    std::optional<SessionDescription> m_sessionDescription;

    /**
     * Id of the session whose connectivity check this ice has punched with
     * (IceUdp::sendBinding). Lets handleStun attribute a binding success
     * response - which carries no USERNAME - to the session for
     * onDirectPathProbeSucceeded. Kept separate from m_sessionDescription so
     * the binding request path still goes through the locked/expired gate.
     */
    std::string m_punchSessionId;

    /** Buffers waiting to be sent; the one being sent is m_sendingBuffer. */
    std::deque<nx::Buffer> m_sendBuffers;
    /** Count of foreground buffers at the head of m_sendBuffers. */
    size_t m_foregroundOffset = 0;
    /**
     * The buffer the socket is sending right now, kept out of m_sendBuffers because the socket
     * holds a pointer to it until completion and a deque insert would move it.
     */
    nx::Buffer m_sendingBuffer;
    bool m_hasBufferToSend = false;
    nx::webrtc::SessionPool* m_sessionPool = nullptr;

    Stage m_stage = binding;

private:
    std::weak_ptr<Session> m_session;
};

} // namespace nx::webrtc
