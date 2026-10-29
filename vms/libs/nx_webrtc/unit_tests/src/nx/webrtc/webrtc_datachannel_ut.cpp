// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include <fstream>

#include <gtest/gtest.h>

#include <nx/utils/log/log.h>
#include <nx/webrtc/webrtc_datachannel.h>

using namespace nx::webrtc;

class TestDataChannel;

enum Type
{
    client,
    server
};

class TestDataChannelStreamer: public AbstractDataChannelDelegate
{
public:
    TestDataChannelStreamer(TestDataChannel* testDataChannel, Type type):
        AbstractDataChannelDelegate(SessionConfig()),
        m_testDataChannel(testDataChannel),
        m_type(type)
    {
    }
    virtual ~TestDataChannelStreamer() { m_dataChannel.reset(); }
    virtual void writeDataChannelPacket(const uint8_t* data, int size);
    virtual void onDataChannelString(const std::string& data, int /*streamId*/)
    {
        m_lastString = data;
        ++m_receivedCount;
    }
    virtual void onDataChannelBinary(const std::string& data, int streamId)
    {
        onDataChannelString(data, streamId);
    }
    bool openDataChannel() { return m_dataChannel.openDataChannel("test-channel", ""); }
    bool writeString(const std::string& string) { return m_dataChannel.writeString(string); }
    bool writeBinary(const std::string& data) { return m_dataChannel.writeBinary(data); }
    bool handlePacket(const uint8_t* data, int size)
    {
        return m_dataChannel.handlePacket(data, size);
    }
    std::string lastString() const { return m_lastString; }
    int receivedCount() const { return m_receivedCount; }

    /// Counts backlog-progress notifications so a test can assert one arrives per
    /// message leaving the queue, not only when the queue reaches empty.
    virtual void onDataChannelQueueProgress() override { ++m_queueProgressCount; }
    int queueProgressCount() const { return m_queueProgressCount; }
    int outputQueueSize() const { return m_dataChannel.outputQueueSize(); }

    virtual void onDataChannelOpened(int streamId) override
    {
        ++m_openedCount;
        m_openedStreamId = streamId;
    }
    int openedCount() const { return m_openedCount; }
    int openedStreamId() const { return m_openedStreamId; }

private:
    int m_queueProgressCount = 0;
    int m_receivedCount = 0;
    int m_openedCount = 0;
    int m_openedStreamId = -1;
    TestDataChannel* m_testDataChannel = nullptr;
    Type m_type = Type::client;
    std::string m_lastString;
};

class TestDataChannel
{
public:
    TestDataChannel():
        m_streamerClient(this, Type::client),
        m_streamerServer(this, Type::server)
    {
    }
    bool writePacket(const uint8_t* data, int size, Type type);
    bool flushQueues();
    bool test();

    TestDataChannelStreamer& client() { return m_streamerClient; }
    TestDataChannelStreamer& server() { return m_streamerServer; }

private:
    TestDataChannelStreamer m_streamerClient;
    TestDataChannelStreamer m_streamerServer;
    std::queue<std::vector<uint8_t>> m_clientServerQueue;
    std::queue<std::vector<uint8_t>> m_serverClientQueue;
};

void TestDataChannelStreamer::writeDataChannelPacket(const uint8_t* data, int size)
{
    m_testDataChannel->writePacket(data, size, m_type);
}

bool TestDataChannel::writePacket(const uint8_t* data, int size, Type type)
{
    // Input of client Streamer is output of server Streamer, and vice versa.
    // But you can't write directly because of deadlock in library,
    // so, emulating asynchronous input/output with queues.
    if (type == Type::server)
    {
        m_serverClientQueue.emplace(data, &data[size]);
    }
    else
    {
        m_clientServerQueue.emplace(data, &data[size]);
    }
    return true;
}

bool TestDataChannel::flushQueues()
{
    while (!m_clientServerQueue.empty() || !m_serverClientQueue.empty())
    {
        auto flush = [](std::queue<std::vector<uint8_t>>& queue, TestDataChannelStreamer& streamer)
        {
            while (!queue.empty())
            {
                auto packet = std::move(queue.front());
                queue.pop();
                if (!streamer.handlePacket(packet.data(), packet.size()))
                    return false;
            }
            return true;
        };
        if (!flush(m_serverClientQueue, m_streamerClient)
            || !flush(m_clientServerQueue, m_streamerServer))
            return false;
    }
    return true;
}

bool TestDataChannel::test()
{
    if (!m_streamerClient.openDataChannel())
        return false;
    if (!flushQueues())
        return false;

    if (!m_streamerServer.writeString("hello"))
        return false;
    if (!flushQueues())
        return false;

    if (m_streamerClient.lastString() != "hello")
        return false;

    if (!m_streamerClient.writeString("world"))
        return false;
    if (!flushQueues())
        return false;

    if (m_streamerServer.lastString() != "world")
        return false;

    if (!m_streamerServer.writeString("lorem"))
        return false;
    if (!flushQueues())
        return false;

    if (m_streamerClient.lastString() != "lorem")
        return false;

    if (!m_streamerClient.writeString("ipsum"))
        return false;
    if (!flushQueues())
        return false;

    if (m_streamerServer.lastString() != "ipsum")
        return false;

    return true;
}

TEST(WebRTC, DataChannel)
{
    TestDataChannel test;
    ASSERT_TRUE(test.test());
}

// A media pump gated on outputQueueSize() cannot see the queue move on its own:
// usrsctp accepts bytes on its own schedule, after the send that filled the queue
// returned. onDataChannelQueueProgress() supplies that wake-up, and it must fire
// per message popped rather than only when the queue hits zero - a pump gated on
// a depth THRESHOLD can become runnable at any depth, and a queue that stalls
// above zero would otherwise never notify it at all.
TEST(WebRTC, DataChannelQueueProgressReportedPerMessageNotOnlyAtEmpty)
{
    TestDataChannel test;
    ASSERT_TRUE(test.client().openDataChannel());
    ASSERT_TRUE(test.flushQueues());

    const int baseline = test.server().queueProgressCount();

    // Above kDataChannelFragment (16 KiB) so writeMessage() splits it into several
    // queue entries - the drain must still be reported once, when the last
    // fragment leaves, not once per fragment. Kept under kSctpMaxMessageSize
    // (64 KiB), which is what the receive path will accept in one message.
    ASSERT_TRUE(test.server().writeString(std::string(40 * 1024, 'x')));
    ASSERT_TRUE(test.flushQueues());

    EXPECT_EQ(test.server().outputQueueSize(), 0) << "queue should have drained";

    // 40 KiB fragments into three messages, so three pops and three
    // notifications. A drain-only implementation would report just one.
    const int notifications = test.server().queueProgressCount() - baseline;
    EXPECT_GE(notifications, 2) << "expected one notification per message popped, got "
                                << notifications;

    // Pumping again with nothing queued pops nothing, so nothing is reported.
    const int settled = test.server().queueProgressCount();
    ASSERT_TRUE(test.flushQueues());
    EXPECT_EQ(test.server().queueProgressCount(), settled)
        << "an idle queue must not report progress";
}

// Both ends must learn the channel became usable: the opener from the peer's
// DCEP ack, the acceptor from the DCEP open itself. A DataPeer session has no
// other signal that the pipe is ready, so a missing callback would leave the
// connected state permanently false on one side.
TEST(WebRTC, DataChannelOpenedReportedOnBothEnds)
{
    TestDataChannel test;
    ASSERT_EQ(test.client().openedCount(), 0);
    ASSERT_EQ(test.server().openedCount(), 0);

    ASSERT_TRUE(test.client().openDataChannel());
    ASSERT_TRUE(test.flushQueues());

    EXPECT_EQ(test.client().openedCount(), 1) << "opener must be notified on DCEP ack";
    EXPECT_EQ(test.server().openedCount(), 1) << "acceptor must be notified on DCEP open";
    EXPECT_EQ(test.client().openedStreamId(), test.server().openedStreamId());
}

// The WsMessage transport queues multi-megabyte protobufs (archive segment
// frames) as single messages: writeMessage() fragments them into 16 KiB SCTP
// chunks with explicit EOR and the receive path reassembles until MSG_EOR, so
// the advertised a=max-message-size does not bound nx-to-nx messages.
TEST(WebRTC, DataChannelLargeMessageRoundTrip)
{
    TestDataChannel test;
    ASSERT_TRUE(test.client().openDataChannel());
    ASSERT_TRUE(test.flushQueues());

    std::string payload(5 * 1024 * 1024, '\0');
    for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = (char) (i * 2654435761u >> 24); //< Uncompressible-ish pattern.

    ASSERT_TRUE(test.client().writeBinary(payload));
    ASSERT_TRUE(test.flushQueues());

    EXPECT_EQ(test.client().outputQueueSize(), 0) << "sender queue should have drained";
    ASSERT_EQ(test.server().lastString().size(), payload.size());
    EXPECT_TRUE(test.server().lastString() == payload) << "payload corrupted in transit";

    // And back, to prove reassembly state fully resets between messages.
    ASSERT_TRUE(test.server().writeBinary(payload));
    ASSERT_TRUE(test.flushQueues());
    ASSERT_EQ(test.client().lastString().size(), payload.size());
    EXPECT_TRUE(test.client().lastString() == payload);
}

// Receiving data must not look like progress on the receiver: only a peer that had
// something queued of its own gets the wake-up. Note the baseline is taken after
// the handshake, because openDataChannel() queues and sends the DCEP open message
// and so legitimately notifies.
// The tail of a large message and the message behind it can reach the socket in one read event:
// both are read, or the second waits for the next packet to raise the event again.
TEST(WebRTC, DataChannelReadsTheMessageBehindALargeOne)
{
    TestDataChannel test;
    ASSERT_TRUE(test.client().openDataChannel());
    ASSERT_TRUE(test.flushQueues());

    ASSERT_TRUE(test.client().writeBinary(std::string(4 * 1024 * 1024, 'v')));
    ASSERT_TRUE(test.client().writeString("after"));
    ASSERT_TRUE(test.flushQueues());

    EXPECT_EQ(test.server().receivedCount(), 2);
    EXPECT_EQ(test.server().lastString(), "after");
}

TEST(WebRTC, DataChannelQueueProgressNotReportedForReceiveOnlyPeer)
{
    TestDataChannel test;
    ASSERT_TRUE(test.client().openDataChannel());
    ASSERT_TRUE(test.flushQueues());

    const int clientBaseline = test.client().queueProgressCount();

    ASSERT_TRUE(test.server().writeString("hello"));
    ASSERT_TRUE(test.flushQueues());
    ASSERT_EQ(test.client().lastString(), "hello");

    EXPECT_EQ(test.client().queueProgressCount(), clientBaseline)
        << "the receiving peer queued nothing, so nothing moved for it";
}
