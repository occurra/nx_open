// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <atomic>
#include <queue>

#include <usrsctp.h>

#include <nx/utils/uuid.h>

#include "webrtc_utils.h"

namespace nx::webrtc {

class AbstractDataChannelDelegate;

class NX_WEBRTC_API DataChannel
{
public:
    /**
     * Messages for the DataChannel establishment protocol (RFC 8832).
     * See https://www.rfc-editor.org/rfc/rfc8832.html
     */
    enum MessageType : uint8_t
    {
        openRequest = 0x00, /**< Unused.*/
        openResponse = 0x01, /**< Unused.*/
        ack = 0x02,
        open = 0x03,
        close = 0x04,
    };

    enum ChannelType : uint8_t
    {
        reliable = 0x00,
        partialReliableRexmit = 0x01,
        partialReliableTimed = 0x02,
    };

private:
    /**
     * NOTE: The order seems to be wrong but these are the actual values.
     * See https://datatracker.ietf.org/doc/html/draft-ietf-rtcweb-data-channel-13#section-8
     */
    enum PayloadId : uint32_t
    {
        control = 50,
        string = 51,
        binaryPart = 52,
        binary = 53,
        stringPart = 54,
        stringEmpty = 56,
        binaryEmpty = 57,
    };

    struct Message
    {
        enum class Type
        {
            string,
            binary,
            control,
            reset,
        };
        Type type = Type::control;
        int streamId = 0;
        std::string data;
        bool eor = true;
        /**
         * Device the message belongs to, for the per-device queue flush on a
         * seek. Null for messages not tied to a device (DCEP control, RTP sync
         * points) - those are never flushed.
         */
        nx::Uuid deviceId;
        /**
         * Send unordered and without retransmissions (SCTP unordered flag +
         * RFC 3758 partial reliability, zero retransmissions). On a lossy path
         * an ordered reliable stream head-of-line blocks every message behind
         * a lost packet for a retransmission round trip - up to rto_min on a
         * timeout - which held overlay metadata past its display window. An
         * unreliable message can afford to just vanish instead: overlay
         * metadata is keyed by its own timestamp and superseded ~30 times a
         * second. DCEP control and RTP sync points stay ordered and reliable.
         */
        bool unreliableTransport = false;
    };

public:
    DataChannel(AbstractDataChannelDelegate* ice, uint16_t sctpPort, const SessionConfig& config);
    ~DataChannel();
    bool handlePacket(const uint8_t* data, int size); /**< Input from the lower transport. */
    /**
     * Output into Data Channel transport. deviceId tags the message for the
     * per-device removeQueuedStrings() flush; null leaves it untagged.
     * unreliableTransport sends unordered and without retransmissions - see
     * Message::unreliableTransport.
     */
    bool writeString(
        const std::string& data,
        int streamId = -1,
        const nx::Uuid& deviceId = {},
        bool unreliableTransport = false);
    bool writeBinary(
        const std::string& data,
        int streamId = -1); /**< Output into Data Channel transport. */
    bool openDataChannel(
        const std::string& label,
        const std::string& protocol); /**< Open Data Channel. */
    bool closeStream(int streamId = -1); /**< Close stream. */
    void reset(); /**< Remove lower transport. */
    int defaultStream() const { return m_defaultStreamId; }

    /**
     * Depth of the output queue, in messages (a message over 16 KiB occupies
     * several entries due to fragmentation). Thread-safe: reads an atomic
     * mirror maintained on the owning thread, so producers on other threads
     * can gate on the backlog.
     */
    int outputQueueSize() const;

    /**
     * Removes the queued-but-unsent string messages tagged with deviceId.
     * Called on the first post-seek packet (BOF), alongside the media-queue
     * flush: the queued strings carry the device's pre-seek overlay metadata,
     * which would only delay the post-seek stream behind stale data. The front
     * entry always stays - it may already be partially handed to usrsctp, and
     * dropping it would leave the peer a truncated fragment chain. Untagged
     * messages and other devices' messages are untouched.
     * @return The number of removed messages.
     */
    int removeQueuedStrings(const nx::Uuid& deviceId);

private:
    bool init();
    /// Outcome of writing one queued message, so the drain loop knows whether another
    /// attempt makes sense.
    enum class WriteResult
    {
        sent, ///< The message left the queue; the next one can be tried.
        wouldBlock, ///< usrsctp took nothing, or only a part: its send buffer is full.
        failed, ///< Unrecoverable send error; the channel has to go down.
    };

    bool handleIo();
    bool handleRead();
    WriteResult handleWrite();
    void notifyQueueProgress();
    void publishQueueSize(); /**< Refresh the atomic outputQueueSize() mirror. */
    bool processReceivedData(int sid, PayloadId ppid);
    bool processReceivedMessage(const Message& message);
    bool processNotification(const union sctp_notification* notify, int len);
    bool processOpenMessage(const Message& message);
    bool sendStreamReset(int streamId);
    bool writeMessage(Message message);
    bool writeUserMessage(Message&& message); /**< Used by writeString() and writeBinary(). */
    bool flushSavedMessages();
    static void usrsctpDebugCallback(const char* format, ...);
    static int usrsctpWriteCallback(
        void* ptr,
        void* data,
        size_t len,
        uint8_t tos,
        uint8_t set_df);
    static void usrsctpUpcallCallback(struct socket*, void* arg, int flags);

private:
    AbstractDataChannelDelegate* m_ice = nullptr;
    uint16_t m_sctpPort = 0;
    const SessionConfig m_config;
    struct socket* m_sctpSocket = nullptr;
    int m_lastUpcallEvents = 0;
    std::vector<uint8_t> m_receiveBuffer;
    std::string m_lastMessage;
    std::string m_partialString;
    std::string m_partialBinary;
    std::string m_partialNotification;
    std::optional<uint16_t> m_negotiatedStreamCount;
    bool m_dataChannelOpened = false;
    int m_defaultStreamId = 0;
    std::string m_label;
    std::string m_protocol;
    std::queue<Message> m_outputQueue;
    std::atomic<int> m_outputQueueSize{0}; /**< Mirror of m_outputQueue.size(). */
    std::queue<Message> m_savedQueue;
};

class NX_WEBRTC_API AbstractDataChannelDelegate
{
public:
    AbstractDataChannelDelegate(const SessionConfig& config);
    virtual ~AbstractDataChannelDelegate();
    virtual void writeDataChannelPacket(const uint8_t* data, int size) = 0;
    virtual void onDataChannelString(const std::string& data, int streamId) = 0;
    virtual void onDataChannelBinary(const std::string& data, int streamId) = 0;

    /**
     * The channel just became usable: either the peer acked our DCEP open, or
     * the peer's own DCEP open arrived. Default no-op - the browser path
     * learns about the channel from the browser side and never needed this;
     * a DataPeer session has no other signal that the pipe is ready.
     */
    virtual void onDataChannelOpened(int /*streamId*/) {}

    /**
     * A message has just left the output queue, so the backlog shrank. Fired on
     * every pop rather than only on the transition to empty, because a delegate
     * gating on a depth threshold has to re-evaluate at each step: the gate can
     * become satisfiable at any depth, and a queue that stalls above zero would
     * otherwise never produce a notification. Needed at all because a media pump
     * gated on outputQueueSize() has no other way to learn the queue moved -
     * usrsctp accepts bytes on its own schedule, after the send that filled it
     * returned.
     */
    virtual void onDataChannelQueueProgress() {}

protected:
    DataChannel m_dataChannel;
};

} // namespace nx::webrtc
