// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "webrtc_datachannel.h"

#include <nx/utils/log/log.h>
#include <nx/utils/scope_guard.h>

#include "webrtc_utils.h"

namespace nx::webrtc {

namespace {

#pragma pack(push, 1)
struct OpenMessage
{
    DataChannel::MessageType type = DataChannel::MessageType::open;
    DataChannel::ChannelType channelType = DataChannel::ChannelType::reliable;
    uint16_t priority;
    uint32_t reliabilityParameter;
    uint16_t labelLength;
    uint16_t protocolLength;
    // The fields with variable length:
    // uint8_t[labelLength] label
    // uint8_t[protocolLength] protocol
};
#pragma pack(pop)

constexpr size_t kDataChannelFragment = 0x4000;
constexpr int kOutputQueueLimit = 1024 * 4; //< Can grow due to fragmentation.

// Messages written before the channel opens: a live start primes every camera's GOP, and each
// pre-roll frame carries a timestamp message. The byte limit is what the output queue holds once
// the saved messages are fragmented.
constexpr size_t kSavedQueueByteLimit = kOutputQueueLimit * kDataChannelFragment;

} // namespace

AbstractDataChannelDelegate::AbstractDataChannelDelegate(const SessionConfig& config):
    m_dataChannel(this, kDefaultSctpPort, config)
{
}

AbstractDataChannelDelegate::~AbstractDataChannelDelegate()
{
}

DataChannel::DataChannel(
    AbstractDataChannelDelegate* ice,
    uint16_t sctpPort,
    const SessionConfig& config):
    m_ice(ice),
    m_sctpPort(sctpPort),
    m_config(config)
{
}

DataChannel::~DataChannel()
{
    if (m_sctpSocket)
    {
        usrsctp_shutdown(m_sctpSocket, SHUT_RDWR);
        usrsctp_close(m_sctpSocket);
        m_sctpSocket = nullptr;
        usrsctp_deregister_address(this);
    }
}

void DataChannel::reset()
{
    m_ice = nullptr;
}

bool DataChannel::init()
{
    // There are too many initialization steps, so the initialization is lazy - only when some DTLS
    // packets are actually received for this ICE Candidate.

    // 1. Init the library.
    static nx::utils::ScopeGuard cleanupLibrary(
        []()
        {
            usrsctp_finish();
        });
    static std::once_flag isLibraryInitializated;
    std::call_once(
        isLibraryInitializated,
        []()
        {
            // usrsctp itself. Note that this call will spawn a thread which will wake each usrsctp
            // socket every 10 ms. This is probably safe, because access to socket is atomic.
            //
            // MUST come before every usrsctp_sysctl_set_...() below: usrsctp_init() runs
            // sctp_init_sysctls(), which resets the whole sysctl block to the library defaults
            // (and then overwrites sendspace/recvspace itself). Settings applied before this
            // call are silently discarded - as this whole block was until VMS-279.
            usrsctp_init(
                /*port*/ 0,
                &DataChannel::usrsctpWriteCallback,
                &DataChannel::usrsctpDebugCallback);
            usrsctp_enable_crc32c_offload(); //< Computing CRC32 only for outgoing packets.

            // usrsctp_sysctl...().
            // The send and receive window size of usrsctp is 256KiB, which is too small for
            // realistic RTTs, therefore we increase it to 1MiB by default for better performance.
            // See https://bugzilla.mozilla.org/show_bug.cgi?id=1051685
            usrsctp_sysctl_set_sctp_recvspace(1024 * 1024);
            usrsctp_sysctl_set_sctp_sendspace(1024 * 1024);

            // Increase the maximum number of chunks in the queue to 10K by default.
            usrsctp_sysctl_set_sctp_max_chunks_on_queue(10 * 1024);

            // Max burst caps how much leaves per output round. With the cwnd-based variant
            // disabled below it no longer touches the congestion window itself.
            usrsctp_sysctl_set_sctp_max_burst_default(64);

            // Initial congestion window. RFC 6928 settles on 10 MTUs for the open internet,
            // but every reconnect starts a fresh association and on a 200 ms path 10 MTUs is
            // an opening capacity of ~58 KB/s - the reconnect burst then becomes a backlog
            // that takes tens of seconds to drain. usrsctp takes the minimum of this and the
            // max burst above, so 64 is the ceiling and asking for more would only mislead.
            usrsctp_sysctl_set_sctp_initial_cwnd(64);

            // Do NOT clamp cwnd to flight_size + max_burst * MTU on every output (the usrsctp
            // default). That clamp assumes a sender that keeps its window full; a frame-paced
            // stream drains between frames, so the clamp pins the window just above the
            // current flight and it never grows. Loss-driven congestion control is unaffected.
            usrsctp_sysctl_set_sctp_use_cwnd_based_maxburst(0);

            // Use the standard SCTP congestion control (RFC 4960) by default.
            // See https://github.com/paullouisageneau/libdatachannel/issues/354
            usrsctp_sysctl_set_sctp_default_cc_module(0);

            // Reduce SACK delay to 20ms by default
            // (the recommended default value from RFC 4960 is 200ms).
            usrsctp_sysctl_set_sctp_delayed_sack_time_default(20);

            // RTO settings.

            // Keep RFC 2988's 1 s floor. A 200 ms floor (what TCP on Linux uses) is safe only
            // on a LAN: toward a cloud peer the RTT is itself ~200 ms, so a floor equal to the
            // RTT makes the retransmission timer expire on ordinary jitter. Each spurious T3
            // timeout resets the congestion window to one MTU, so the window sawtooths between
            // 30 and 100 KB instead of reaching the ~400 KB the link sustains, and the send
            // buffer backs up behind it - seconds of added latency with nothing lost.
            usrsctp_sysctl_set_sctp_rto_min_default(1'000);
            // Set only 10s as max RTO instead of 60s for shorter connection timeout.
            usrsctp_sysctl_set_sctp_rto_max_default(10'000);
            usrsctp_sysctl_set_sctp_init_rto_max_default(10'000);
            // Set 1s as initial RTO like for min RTO.
            usrsctp_sysctl_set_sctp_rto_initial_default(1'000);

            // RTX settings.

            // Attempts before the path is declared dead. Read together with the min RTO
            // above: the backoff doubles from 1 s, so 4 attempts give up after ~15 s, which
            // is long enough to ride out an ordinary blip and short enough not to hold on to
            // a path that is genuinely gone. 8, the library default, would be ~55 s.
            auto maxRtx = 4;
            usrsctp_sysctl_set_sctp_init_rtx_max_default(maxRtx);
            usrsctp_sysctl_set_sctp_assoc_rtx_max_default(maxRtx);
            usrsctp_sysctl_set_sctp_path_rtx_max_default(maxRtx); //< Single path.

            // Heartbeat interval.
            usrsctp_sysctl_set_sctp_heartbeat_interval_default(10000);

            usrsctp_sysctl_set_sctp_pr_enable(
                1); //< Enable Partial Reliability Extension (RFC 3758).
            usrsctp_sysctl_set_sctp_ecn_enable(0); //< Disable Explicit Congestion Notification.
        // Uncomment if you need a verbose debug output with usrsctp internals.
#if 0
            usrsctp_sysctl_set_sctp_debug_on(SCTP_DEBUG_ALL);
#endif
        });

    // 2. Init the socket.
    usrsctp_register_address(this);
    m_sctpSocket = usrsctp_socket(
        AF_CONN,
        SOCK_STREAM,
        IPPROTO_SCTP,
        /*receive_cb*/ nullptr,
        /*send_cb*/ nullptr,
        /*sb_threshold*/ 0,
        /*ulp_info*/ nullptr);
    if (!m_sctpSocket)
    {
        NX_DEBUG(this, "Can't create SCTP socket: errno %1", errno);
        return false;
    }
    usrsctp_set_upcall(m_sctpSocket, &DataChannel::usrsctpUpcallCallback, this);

    if (usrsctp_set_non_blocking(m_sctpSocket, /*onoff*/ 1))
    {
        NX_DEBUG(this, "Can't set nonblocking SCTP socket: errno %1", errno);
        return false;
    }

    // SCTP must stop sending after the lower layer is shut down, so disable the linger.
    struct linger socketLinger = {};
    socketLinger.l_onoff = 1;
    socketLinger.l_linger = 0;
    if (usrsctp_setsockopt(
            m_sctpSocket,
            SOL_SOCKET,
            SO_LINGER,
            &socketLinger,
            sizeof(socketLinger)))
    {
        NX_DEBUG(this, "Can't set SO_LINGER on SCTP socket: errno %1", errno);
        return false;
    }

    struct sctp_assoc_value sctpAssocValue = {};
    sctpAssocValue.assoc_id = SCTP_ALL_ASSOC;
    sctpAssocValue.assoc_value = 1;
    if (usrsctp_setsockopt(
            m_sctpSocket,
            IPPROTO_SCTP,
            SCTP_ENABLE_STREAM_RESET,
            &sctpAssocValue,
            sizeof(sctpAssocValue)))
    {
        NX_DEBUG(this, "Can't set SCTP_ENABLE_STREAM_RESET on SCTP socket: errno %1", errno);
        return false;
    }
    int isOn = 1;
    if (usrsctp_setsockopt(m_sctpSocket, IPPROTO_SCTP, SCTP_RECVRCVINFO, &isOn, sizeof(isOn)))
    {
        NX_DEBUG(this, "Can't set SCTP_RECVRCVINFO on SCTP socket: errno %1", errno);
        return false;
    }

    // Set explicit EOR.
    if (usrsctp_setsockopt(m_sctpSocket, IPPROTO_SCTP, SCTP_EXPLICIT_EOR, &isOn, sizeof(isOn)))
    {
        NX_DEBUG(this, "Can't set SCTP_EXPLICIT_EOR on SCTP socket: errno %1", errno);
        return false;
    }

    struct sctp_event sctpEvent = {};
    sctpEvent.se_assoc_id = SCTP_ALL_ASSOC;
    sctpEvent.se_on = 1;
    sctpEvent.se_type = SCTP_ASSOC_CHANGE;
    if (usrsctp_setsockopt(m_sctpSocket, IPPROTO_SCTP, SCTP_EVENT, &sctpEvent, sizeof(sctpEvent)))
    {
        NX_DEBUG(
            this,
            "Can't set subscription to SCTP_ASSOC_CHANGE for SCTP socket: errno %1",
            errno);
        return false;
    }

    sctpEvent.se_type = SCTP_SENDER_DRY_EVENT;
    if (usrsctp_setsockopt(m_sctpSocket, IPPROTO_SCTP, SCTP_EVENT, &sctpEvent, sizeof(sctpEvent)))
    {
        NX_DEBUG(
            this,
            "Can't set subscription to SCTP_SENDER_DRY_EVENT for SCTP socket: errno %1",
            errno);
        return false;
    }
    sctpEvent.se_type = SCTP_STREAM_RESET_EVENT;
    if (usrsctp_setsockopt(m_sctpSocket, IPPROTO_SCTP, SCTP_EVENT, &sctpEvent, sizeof(sctpEvent)))
    {
        NX_DEBUG(
            this,
            "Can't set subscription to SCTP_STREAM_RESET_EVENT for SCTP socket: errno %1",
            errno);
        return false;
    }

    // RFC 8831 6.6. Transferring User Data on a Data Channel
    // The sender SHOULD disable the Nagle algorithm (see [RFC1122) to minimize the latency.
    // See https://www.rfc-editor.org/rfc/rfc8831.html#section-6.6
    int noDelay = 1;
    if (usrsctp_setsockopt(m_sctpSocket, IPPROTO_SCTP, SCTP_NODELAY, &noDelay, sizeof(noDelay)))
    {
        NX_DEBUG(this, "Can't set SCTP_NODELAY for SCTP socket: errno %1", errno);
        return false;
    }

    struct sctp_paddrparams paddrparams = {};
    // Enable SCTP heartbeats
    paddrparams.spp_flags = SPP_HB_ENABLE;

    // RFC 8261 5. DTLS considerations:
    // If path MTU discovery is performed by the SCTP layer and IPv4 is used as the network-layer
    // protocol, the DTLS implementation SHOULD allow the DTLS user to enforce that the
    // corresponding IPv4 packet is sent with the Don't Fragment (DF) bit set. If controlling the
    // DF bit is not possible (for example, due to implementation restrictions), a safe value for
    // the path MTU has to be used by the SCTP stack. It is RECOMMENDED that the safe value not
    // exceed 1200 bytes. See https://www.rfc-editor.org/rfc/rfc8261.html#section-5 Fall back to a
    // safe MTU value.
    paddrparams.spp_flags |= SPP_PMTUD_DISABLE;
    // The MTU value provided specifies the space available for chunks in the
    // packet, so we also subtract the SCTP header size.
    constexpr size_t kDefaultSctpMtu =
        /*MTU*/ 1280 - /*SCTP*/ 12 - /*DTLS*/ 48 - /*UDP*/ 8 - /*IPv6*/ 40;
    paddrparams.spp_pathmtu = kDefaultSctpMtu;

    if (usrsctp_setsockopt(
            m_sctpSocket,
            IPPROTO_SCTP,
            SCTP_PEER_ADDR_PARAMS,
            &paddrparams,
            sizeof(paddrparams)))
    {
        NX_DEBUG(this, "Can't set SCTP_PEER_ADDR_PARAMS for SCTP socket: errno %1", errno);
        return false;
    }

    // RFC 8831 6.2. SCTP Association Management
    // The number of streams negotiated during SCTP association setup SHOULD be 65535, which is the
    // maximum number of streams that can be negotiated during the association setup.
    // See https://www.rfc-editor.org/rfc/rfc8831.html#section-6.2
    // However, usrsctp allocates tables to hold the stream states. For 65535 streams, it results
    // in the waste of a few MBs for each association. Therefore, we use a lower limit to save
    // memory. See https://github.com/sctplab/usrsctp/issues/121
    struct sctp_initmsg initMsg = {};
    constexpr int kMaxSctpStreamsCount = 1024;
    initMsg.sinit_num_ostreams = kMaxSctpStreamsCount;
    initMsg.sinit_max_instreams = kMaxSctpStreamsCount;
    if (usrsctp_setsockopt(m_sctpSocket, IPPROTO_SCTP, SCTP_INITMSG, &initMsg, sizeof(initMsg)))
    {
        NX_DEBUG(this, "Can't set SCTP_INITMSG for SCTP socket: errno %1", errno);
        return false;
    }

    // Prevent fragmented interleave of messages (i.e. level 0), see RFC 6458 section 8.1.20.
    // Unless the user has set the fragmentation interleave level to 0, notifications
    // may also be interleaved with partially delivered messages.
    int level = 0;
    if (usrsctp_setsockopt(
            m_sctpSocket,
            IPPROTO_SCTP,
            SCTP_FRAGMENT_INTERLEAVE,
            &level,
            sizeof(level)))
    {
        NX_DEBUG(this, "Can't disable SCTP fragmented interleave: errno %1", errno);
        return false;
    }

    int rcvBuf = 0;
    socklen_t rcvBufLen = sizeof(rcvBuf);
    if (usrsctp_getsockopt(m_sctpSocket, SOL_SOCKET, SO_RCVBUF, &rcvBuf, &rcvBufLen))
    {
        NX_DEBUG(this, "Can't get SCTP recv buffer size for SCTP socket: errno %1", errno);
        return false;
    }

    int sndBuf = 0;
    socklen_t sndBufLen = sizeof(sndBuf);
    if (usrsctp_getsockopt(m_sctpSocket, SOL_SOCKET, SO_SNDBUF, &sndBuf, &sndBufLen))
    {
        NX_DEBUG(this, "Can't get SCTP send buffer size for SCTP socket: errno %1", errno);
        return false;
    }

    // Ensure the buffer is also large enough to accommodate the largest messages.
    constexpr int kMaxMessageSize = 256 * 1024;
    rcvBuf = std::max(rcvBuf, kMaxMessageSize);
    sndBuf = std::max(sndBuf, kMaxMessageSize);

    if (usrsctp_setsockopt(m_sctpSocket, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof(rcvBuf)))
    {
        NX_DEBUG(this, "Can't set SCTP recv buffer size for SCTP socket: errno %1", errno);
        return false;
    }

    if (usrsctp_setsockopt(m_sctpSocket, SOL_SOCKET, SO_SNDBUF, &sndBuf, sizeof(sndBuf)))
    {
        NX_DEBUG(this, "Can't set SCTP send buffer size for SCTP socket: errno %1", errno);
        return false;
    }

    // 3. Connect the socket.
    struct sockaddr_conn sconn = {};
    sconn.sconn_family = AF_CONN;
    sconn.sconn_port = htons(m_sctpPort);
    sconn.sconn_addr = this;
#ifdef HAVE_SCONN_LEN
    sconn.sconn_len = sizeof(sconn);
#endif
    if (usrsctp_bind(m_sctpSocket, (struct sockaddr*) &sconn, sizeof(sconn)))
    {
        NX_DEBUG(this, "Can't bind socket: errno %1", errno);
        return false;
    }
    int ret = usrsctp_connect(m_sctpSocket, (struct sockaddr*) &sconn, sizeof(sconn));
    if (ret && errno != EINPROGRESS)
    {
        NX_DEBUG(this, "Can't connect socket: errno %1, return code %2", errno, ret);
        return false;
    }
    return true;
}

void DataChannel::usrsctpDebugCallback(const char* /*format*/, ...)
{
    // Not implemented.
}

int DataChannel::usrsctpWriteCallback(
    void* ptr,
    void* data,
    size_t len,
    uint8_t /*tos*/,
    uint8_t /*set_df*/)
{
    auto* dataChannel = static_cast<DataChannel*>(ptr);

    if (!dataChannel->m_ice)
        return -1; //< Failure.
    // Set the CRC32 ourselves as we have enabled CRC32 offloading.
    if (len >= 12)
    {
        uint32_t* checksum = (uint32_t*) data + 2;
        *checksum = 0;
        *checksum = usrsctp_crc32c(data, len);
    }
    NX_TRACE(dataChannel, "Write callback called with len %1", len);
    dataChannel->m_ice->writeDataChannelPacket((uint8_t*) data, len);
    return 0; //< Success.
}

void DataChannel::usrsctpUpcallCallback(struct socket*, void* arg, int /*flags*/)
{
    auto* dataChannel = static_cast<DataChannel*>(arg);

    if (!dataChannel->m_ice)
        return;

    NX_CRITICAL(dataChannel->m_sctpSocket);
    dataChannel->m_lastUpcallEvents = usrsctp_get_events(dataChannel->m_sctpSocket);
    NX_TRACE(
        dataChannel,
        "Upcall: %1 (%2%3)",
        dataChannel->m_lastUpcallEvents,
        ((dataChannel->m_lastUpcallEvents & SCTP_EVENT_READ) ? "r" : ""),
        ((dataChannel->m_lastUpcallEvents & SCTP_EVENT_WRITE) ? "w" : ""));
}

bool DataChannel::handleRead()
{
    for (;;)
    {
        if (m_receiveBuffer.size() != kSctpMaxMessageSize)
            m_receiveBuffer.resize(kSctpMaxMessageSize);
        socklen_t fromLen = 0;
        struct sctp_rcvinfo info = {};
        socklen_t infoLen = sizeof(info);
        unsigned int infoType = 0;
        int flags = 0;
        ssize_t len = usrsctp_recvv(
            m_sctpSocket,
            m_receiveBuffer.data(),
            kSctpMaxMessageSize,
            /*from*/ nullptr,
            &fromLen,
            &info,
            &infoLen,
            &infoType,
            &flags);
        //< A partial chunk of a message larger than the buffer fills it
        //< completely, so equality is legitimate; recvv cannot return more.
        NX_CRITICAL(len <= kSctpMaxMessageSize);

        const auto savedErrno = errno;

        NX_TRACE(this, "SCTP recv: got len %1, errno %2", len, savedErrno);
        if (len < 0)
        {
            if (savedErrno == EWOULDBLOCK || savedErrno == EAGAIN || savedErrno == ECONNRESET)
                return true; //< Not a fatal error.
            else
                return false;
        }

        // SCTP_FRAGMENT_INTERLEAVE does not seem to work as expected for messages > 64KB,
        // therefore partial notifications and messages need to be handled separately.
        if (flags & MSG_NOTIFICATION)
        {
            m_partialNotification.insert(
                m_partialNotification.size(),
                (char*) m_receiveBuffer.data(),
                len);
            if (flags & MSG_EOR)
            {
                // Notification is complete, process it.
                auto notification = (union sctp_notification*) m_partialNotification.data();
                if (!processNotification(notification, m_partialNotification.size()))
                    return false;
                m_partialNotification.clear();
            }
        }
        else
        {
            // SCTP message.
            m_lastMessage.insert(m_lastMessage.size(), (char*) m_receiveBuffer.data(), len);
            if (flags & MSG_EOR)
            {
                // Message is complete, process it.
                if (infoType != SCTP_RECVV_RCVINFO)
                {
                    NX_DEBUG(this, "Missing SCTP recv info");
                    return false;
                }
                bool openedBefore = m_dataChannelOpened;
                bool ret =
                    processReceivedData((int) info.rcv_sid, (PayloadId) ntohl(info.rcv_ppid));
                m_lastMessage.clear();
                if (!openedBefore && m_dataChannelOpened)
                    ret = ret && flushSavedMessages();
                if (!ret)
                    return false;
            }
        }
    }
    return true;
}

int DataChannel::outputQueueSize() const
{
    return m_outputQueueSize.load(std::memory_order_relaxed);
}

int DataChannel::removeQueuedStrings(const nx::Uuid& deviceId)
{
    if (deviceId.isNull() || m_outputQueue.size() <= 1)
        return 0;

    std::queue<Message> kept;
    kept.push(std::move(m_outputQueue.front()));
    m_outputQueue.pop();
    int removed = 0;
    while (!m_outputQueue.empty())
    {
        auto& message = m_outputQueue.front();
        const bool isUserMessage = message.type == Message::Type::string
            || message.type == Message::Type::binary;
        if (isUserMessage && message.deviceId == deviceId)
            ++removed;
        else
            kept.push(std::move(message));
        m_outputQueue.pop();
    }
    m_outputQueue = std::move(kept);
    publishQueueSize();
    return removed;
}

void DataChannel::publishQueueSize()
{
    m_outputQueueSize.store((int) m_outputQueue.size(), std::memory_order_relaxed);
}

void DataChannel::notifyQueueProgress()
{
    publishQueueSize();
    // Every pop, not just the transition to empty. A delegate that gates on a
    // backlog THRESHOLD rather than on emptiness needs the condition
    // re-evaluated each time the queue shrinks: it can become satisfiable at any
    // depth, and if the queue then stalls above zero a drain-only notification
    // would never arrive and the delegate would wait forever.
    if (m_ice)
        m_ice->onDataChannelQueueProgress();
}

DataChannel::WriteResult DataChannel::handleWrite()
{
    NX_CRITICAL(m_outputQueue.size() > 0);
    auto& message = m_outputQueue.front();
    uint32_t payloadId = 0;
    auto streamId = message.streamId;
    switch (message.type)
    {
        case Message::Type::string:
            payloadId = (message.data.empty() ? PayloadId::stringEmpty : PayloadId::string);
            break;
        case Message::Type::binary:
            payloadId = (message.data.empty() ? PayloadId::binaryEmpty : PayloadId::binary);
            break;
        case Message::Type::control:
            payloadId = PayloadId::control;
            break;
        case Message::Type::reset:
            m_outputQueue.pop();
            notifyQueueProgress();
            return sendStreamReset(streamId) ? WriteResult::sent : WriteResult::failed;
        default:
            NX_CRITICAL(false);
            break;
    }

    // Always use default reliability parameters: looks like all dataChannel implementations
    // always use TCP-like reliability.

    struct sctp_sendv_spa sendvSpa = {};

    // Set sndinfo.
    sendvSpa.sendv_flags |= SCTP_SEND_SNDINFO_VALID;
    sendvSpa.sendv_sndinfo.snd_sid = uint16_t(message.streamId);
    sendvSpa.sendv_sndinfo.snd_ppid = htonl(payloadId);
    if (message.eor)
        sendvSpa.sendv_sndinfo.snd_flags |= SCTP_EOR;
    else
        sendvSpa.sendv_sndinfo.snd_flags &= ~SCTP_EOR;
    // Deliberately narrower than the prinfo condition below: the session-wide
    // m_config.unreliableTransport (the tracker's MSE media-over-datachannel
    // mode) keeps its historical "no retransmissions, but ordered" contract -
    // an MSE consumer appends segments in order. Only the per-message flag,
    // used for self-contained overlay metadata, also drops the ordering.
    if (message.unreliableTransport)
        sendvSpa.sendv_sndinfo.snd_flags |= SCTP_UNORDERED;

    // Set prinfo. The PR-SCTP policy names read inverted: SCTP_PR_SCTP_NONE
    // means no PARTIAL-reliability policy - i.e. fully reliable delivery with
    // unlimited retransmissions - while SCTP_PR_SCTP_RTX caps retransmissions
    // at pr_value, and pr_value = 0 sends exactly once: a lost chunk is
    // abandoned via FORWARD-TSN instead of being retransmitted.
    sendvSpa.sendv_flags |= SCTP_SEND_PRINFO_VALID;

    if (m_config.unreliableTransport || message.unreliableTransport)
        sendvSpa.sendv_prinfo.pr_policy = SCTP_PR_SCTP_RTX;
    else
        sendvSpa.sendv_prinfo.pr_policy = SCTP_PR_SCTP_NONE;
    sendvSpa.sendv_prinfo.pr_value = 0;

    ssize_t ret = 0;
    if (!message.data.empty())
    {
        ret = usrsctp_sendv(
            m_sctpSocket,
            message.data.data(),
            message.data.size(),
            /*to*/ nullptr,
            /*addrcnt*/ 0,
            &sendvSpa,
            sizeof(sendvSpa),
            SCTP_SENDV_SPA,
            /*flags*/ 0);
    }
    else
    {
        const char zero = 0;
        ret = usrsctp_sendv(
            m_sctpSocket,
            /*data*/ &zero,
            /*size*/ 1,
            /*to*/ nullptr,
            /*addrcnt*/ 0,
            &sendvSpa,
            sizeof(sendvSpa),
            SCTP_SENDV_SPA,
            /*flags*/ 0);
    }

    if (ret < 0)
    {
        if (errno == EWOULDBLOCK || errno == EAGAIN)
        {
            NX_DEBUG(this, "SCTP send buffer is full");
            return WriteResult::wouldBlock;
        }
        else
        {
            NX_DEBUG(this, "SCTP sending failed: errno %1", errno);
            return WriteResult::failed;
        }
    }
    NX_TRACE(this, "SCTP send size = %1 ret = %2", message.data.size(), ret);
    if (message.data.size() == (size_t) ret)
    {
        m_outputQueue.pop();
        notifyQueueProgress();
        return WriteResult::sent;
    }

    // usrsctp accepted a prefix and stopped, which means its buffer just filled up. Keep
    // the remainder at the head of the queue for the next write.
    NX_ASSERT((size_t) ret < message.data.size());
    message.data.erase(0, ret);
    return WriteResult::wouldBlock;
}

bool DataChannel::sendStreamReset(int streamId)
{
    const size_t len = sizeof(struct sctp_reset_streams) + sizeof(uint16_t);
    uint8_t buffer[len] = {};
    struct sctp_reset_streams& resetStreams = *(struct sctp_reset_streams*) buffer;
    resetStreams.srs_flags = SCTP_STREAM_RESET_OUTGOING;
    resetStreams.srs_number_streams = 1;
    resetStreams.srs_stream_list[0] = streamId;

    if (usrsctp_setsockopt(m_sctpSocket, IPPROTO_SCTP, SCTP_RESET_STREAMS, &resetStreams, len)
        == 0)
        return true;
    if (errno == EINVAL)
    {
        NX_VERBOSE(this, "SCTP stream %1 already reset", streamId);
        return true; //< Not a fatal error.
    }
    else
    {
        NX_VERBOSE(this, "SCTP reset stream %1 failed, errno %2", streamId, errno);
        return false;
    }
}

bool DataChannel::handleIo()
{
    NX_CRITICAL(m_sctpSocket);

    bool handled = false;
    do
    {
        handled = false;
        if (m_lastUpcallEvents & SCTP_EVENT_READ)
        {
            handled = true;
            // Upcall callback can be called with new event set on usrsctp_recvv.
            m_lastUpcallEvents &= ~SCTP_EVENT_READ;
            if (!handleRead())
                return false;
        }
        if ((m_lastUpcallEvents & SCTP_EVENT_WRITE) && m_outputQueue.size() > 0)
        {
            handled = true;
            // Upcall callback can be called with new event set on usrsctp_sendv.
            m_lastUpcallEvents &= ~SCTP_EVENT_WRITE;
            // Hand usrsctp everything it will take rather than one message per event: it
            // applies the congestion window itself and stops accepting once its send
            // buffer is full. Sending a single message per upcall caps the channel at the
            // upcall rate and, worse, starves the association - with the data waiting in
            // this queue instead of in flight, the congestion window never fills, so it
            // never grows, and everything queued behind it is delayed, including messages
            // that have nothing to do with video.
            while (!m_outputQueue.empty())
            {
                const WriteResult result = handleWrite();
                if (result == WriteResult::failed)
                    return false;
                if (result == WriteResult::wouldBlock)
                    break;
            }
        }
        // Loop until the internal usrsctp event queue is empty, and there is no data to send
        // in the outputQueue.
    } while (handled);
    return true;
}

bool DataChannel::closeStream(int streamId)
{
    if (!m_dataChannelOpened)
        return true;
    if (streamId < 0)
        streamId = defaultStream();
    m_dataChannelOpened = false;
    // RFC 8831 6.7. Closing a Data Channel
    // Closing of a data channel MUST be signaled by resetting the corresponding outgoing streams.
    // See https://www.rfc-editor.org/rfc/rfc8831.html#section-6.7
    return writeMessage({Message::Type::reset, streamId, std::string(), true});
}

bool DataChannel::writeString(
    const std::string& data,
    int streamId,
    const nx::Uuid& deviceId,
    bool unreliableTransport)
{
    if (streamId < 0)
        streamId = defaultStream();
    return writeUserMessage(
        {Message::Type::string, streamId, data, true, deviceId, unreliableTransport});
}

bool DataChannel::writeBinary(
    std::string data,
    int streamId,
    const nx::Uuid& deviceId,
    bool unreliableTransport)
{
    if (streamId < 0)
        streamId = defaultStream();
    return writeUserMessage(
        {Message::Type::binary, streamId, std::move(data), true, deviceId, unreliableTransport});
}

bool DataChannel::flushSavedMessages()
{
    NX_ASSERT(m_dataChannelOpened);
    while (!m_savedQueue.empty())
    {
        if (!writeMessage(m_savedQueue.front()))
            return false;
        m_savedQueueBytes -= m_savedQueue.front().data.size();
        m_savedQueue.pop();
    }
    return true;
}

bool DataChannel::writeUserMessage(Message&& message)
{
    if (!m_dataChannelOpened)
    {
        if (m_savedQueue.size() >= kOutputQueueLimit
            || m_savedQueueBytes + message.data.size() > kSavedQueueByteLimit)
        {
            NX_DEBUG(this, "Saved queue overflow: %1 messages, %2 bytes",
                m_savedQueue.size(), m_savedQueueBytes);
            return false;
        }
        NX_TRACE(this, "Saved message with type %1", (int) message.type);
        m_savedQueueBytes += message.data.size();
        m_savedQueue.emplace(std::move(message));
        return true;
    }
    else
    {
        return writeMessage(message);
    }
}

bool DataChannel::writeMessage(Message message)
{
    if (!m_sctpSocket && !init())
        return false;

    if (m_outputQueue.size() >= kOutputQueueLimit)
    {
        NX_DEBUG(this, "Output queue size overflow: %1", m_outputQueue.size());
        return false;
    }
    NX_VERBOSE(
        this,
        "Added message with type %1 size %2",
        (int) message.type,
        message.data.size());
    m_lastUpcallEvents |= SCTP_EVENT_WRITE;
    if (message.data.size() <= kDataChannelFragment)
    {
        m_outputQueue.emplace(std::move(message));
        publishQueueSize();
        return handleIo();
    }

    size_t offset = 0;
    while (offset != message.data.size())
    {
        // usrsctp don't support a large messages (>~200k) due to its internal buffer limitations.
        // So we need to fragment message first.
        size_t len = std::min(kDataChannelFragment, message.data.size() - offset);
        // Fragment original message.
        Message fragmentedMessage{
            message.type,
            message.streamId,
            {message.data.data() + offset, len},
            len + offset == message.data.size() ? message.eor : false,
            message.deviceId,
            message.unreliableTransport};
        m_outputQueue.emplace(std::move(fragmentedMessage));
        offset += len;
    }
    publishQueueSize();
    return handleIo();
}

bool DataChannel::handlePacket(const uint8_t* data, int size)
{
    if (!m_sctpSocket && !init())
        return false;

    // So strange API. Add some data to socket, then recv from this socket.
    usrsctp_conninput(this, data, size, /*ecn_bits*/ 0);

    return handleIo();
}

bool DataChannel::processReceivedData(int streamId, PayloadId payloadId)
{
    // RFC 8831: The usage of the PPIDs "WebRTC String Partial" and "WebRTC Binary Partial" is
    // deprecated. They were used for a PPID-based fragmentation and reassembly of user messages
    // belonging to reliable and ordered data channels.
    // See https://www.rfc-editor.org/rfc/rfc8831.html#section-6.6.
    // We handle those PPIDs at reception for compatibility reasons but shall never send them.
    switch (payloadId)
    {
        case control:
        {
            NX_VERBOSE(this, "Got control, type = %1", (int) m_lastMessage[0]);
            return processReceivedMessage({Message::Type::control, streamId, m_lastMessage});
        }

        case stringPart: //< Deprecated.
        {
            m_partialString.insert(m_partialString.size(), m_lastMessage);
            break;
        }

        case string:
        {
            if (m_partialString.empty())
            {
                return processReceivedMessage({Message::Type::string, streamId, m_lastMessage});
            }
            else
            {
                std::string tempstr(m_partialString);
                tempstr.insert(tempstr.size(), m_lastMessage);
                m_partialString.clear();
                return processReceivedMessage({Message::Type::string, streamId, tempstr, true});
            }
            break;
        }

        case stringEmpty:
        {
            // Don't report into the dataChannel level.
            m_partialString.clear();
            break;
        }

        case binaryPart: //< Deprecated.
        {
            m_partialBinary.insert(m_partialBinary.size(), m_lastMessage);
            break;
        }

        case binary:
        {
            if (m_partialBinary.empty())
            {
                return processReceivedMessage(
                    {Message::Type::binary, streamId, m_lastMessage, true});
            }
            else
            {
                std::string tempbin(m_partialBinary);
                tempbin.insert(tempbin.size(), m_lastMessage);
                m_partialBinary.clear();
                return processReceivedMessage({Message::Type::binary, streamId, tempbin, true});
            }
            break;
        }

        case binaryEmpty:
        {
            // Don't report into the dataChannel level.
            m_partialBinary.clear();
            break;
        }

        default:
        {
            // Unknown
            NX_DEBUG(this, "Unknown PPID: %1", (uint32_t) payloadId);
            break;
        }
    }
    return true;
}

bool DataChannel::processNotification(const union sctp_notification* notification, int len)
{
    if (len < (int) sizeof(struct sctp_notification::sctp_tlv)
        || len != (int) notification->sn_header.sn_length)
    {
        NX_DEBUG(this, "Got bad notification with length %1", len);
        return true; //< Not a fatal error.
    }

    auto type = notification->sn_header.sn_type;
    NX_VERBOSE(this, "Processing notification, type: %1", type);

    switch (type)
    {
        case SCTP_ASSOC_CHANGE:
        {
            NX_VERBOSE(this, "SCTP association change event");
            const struct sctp_assoc_change& assocChange = notification->sn_assoc_change;
            if (assocChange.sac_state == SCTP_COMM_UP)
            {
                NX_VERBOSE(
                    this,
                    "SCTP negotiated streams: incoming = %1, outgoing = %2",
                    assocChange.sac_inbound_streams,
                    assocChange.sac_outbound_streams);
                // Not used at this moment.
                m_negotiatedStreamCount.emplace(
                    std::min(assocChange.sac_inbound_streams, assocChange.sac_outbound_streams));
                return true;
            }
            else
            {
                NX_DEBUG(this, "SCTP negotiation failed or closed");
                return false;
            }
            break;
        }

        case SCTP_SENDER_DRY_EVENT:
        {
            NX_TRACE(this, "SCTP sender dry event");
            // Do nothing, write() will be already called.
            break;
        }

        case SCTP_STREAM_RESET_EVENT:
        {
            const struct sctp_stream_reset_event& resetEvent = notification->sn_strreset_event;
            const int count = (resetEvent.strreset_length - sizeof(resetEvent)) / sizeof(uint16_t);
            const uint16_t flags = resetEvent.strreset_flags;

            {
                std::ostringstream description;
                description << "flags=";
                if ((flags & SCTP_STREAM_RESET_OUTGOING_SSN && flags)
                    & SCTP_STREAM_RESET_INCOMING_SSN)
                {
                    description << "outgoing|incoming";
                }
                else if (flags & SCTP_STREAM_RESET_OUTGOING_SSN)
                {
                    description << "outgoing";
                }
                else if (flags & SCTP_STREAM_RESET_INCOMING_SSN)
                {
                    description << "incoming";
                }
                else
                {
                    description << "0";
                }

                description << ", streams=[";
                for (int i = 0; i < count; ++i)
                {
                    uint16_t streamId = resetEvent.strreset_stream_list[i];
                    description << (i != 0 ? "," : "") << streamId;
                }
                description << "]";

                NX_VERBOSE(this, "SCTP reset event: %1", description.str());
            }

            // RFC 8831 6.7. Closing a Data Channel.
            // If one side decides to close the data channel, it resets the corresponding outgoing
            // stream. When the peer sees that an incoming stream was reset, it also resets its
            // corresponding outgoing stream.
            // See https://www.rfc-editor.org/rfc/rfc8831.html#section-6.7
            if (flags & SCTP_STREAM_RESET_INCOMING_SSN)
            {
                const char dataChannelCloseMessage{(char) MessageType::close};
                for (int i = 0; i < count; ++i)
                {
                    uint16_t streamId = resetEvent.strreset_stream_list[i];
                    // Pass reset as control with `close` message into dataChannel level.
                    return processReceivedMessage(
                        {Message::Type::control,
                         streamId,
                         std::string(&dataChannelCloseMessage, &dataChannelCloseMessage + 1),
                         true});
                }
            }
            break;
        }

        default:
            // Ignore.
            break;
    }
    return true;
}

bool DataChannel::processReceivedMessage(const Message& message)
{
    switch (message.type)
    {
        case Message::Type::control:
        {
            if (message.data.empty())
                return true; //< Ignore.
            // It's a DCEP level.
            uint8_t raw = (uint8_t) message.data[0];
            switch (raw)
            {
                case MessageType::open:
                    return processOpenMessage(message);
                case MessageType::ack:
                    m_dataChannelOpened = true;
                    NX_DEBUG(
                        this,
                        "Opened local Data Channel with stream %1, label %2, protocol %3",
                        m_defaultStreamId,
                        m_label,
                        m_protocol);
                    if (m_ice)
                        m_ice->onDataChannelOpened(m_defaultStreamId);
                    break;
                case MessageType::close:
                    m_dataChannelOpened = false;
                    break;
                default:
                    break;
            }
            break;
        }
        case Message::Type::reset:
            // Unused type, `reset` is handled as `control`.
            break;
        case Message::Type::string:
            NX_ASSERT(!message.data.empty()); //< Unexpected at dataChannel level.
            if (m_ice)
                m_ice->onDataChannelString(message.data, message.streamId);
            break;
        case Message::Type::binary:
            NX_ASSERT(!message.data.empty()); //< Unexpected at dataChannel level.
            if (m_ice)
                m_ice->onDataChannelBinary(message.data, message.streamId);
            break;
    }
    return true;
}

bool DataChannel::processOpenMessage(const Message& message)
{
    if (message.data.size() < sizeof(OpenMessage))
    {
        NX_DEBUG(
            this,
            "Invalid open message size: %1 vs %2",
            message.data.size(),
            sizeof(OpenMessage));
        return false;
    }

    OpenMessage openMessage = *(const OpenMessage*) message.data.data();
    openMessage.priority = ntohs(openMessage.priority);
    openMessage.reliabilityParameter = ntohl(openMessage.reliabilityParameter);
    openMessage.labelLength = ntohs(openMessage.labelLength);
    openMessage.protocolLength = ntohs(openMessage.protocolLength);

    if (message.data.size()
        < sizeof(OpenMessage) + (size_t) (openMessage.labelLength + openMessage.protocolLength))
    {
        NX_DEBUG(
            this,
            "Truncated open message size: %1 vs %2",
            message.data.size(),
            sizeof(OpenMessage) + (size_t) (openMessage.labelLength + openMessage.protocolLength));
        return false;
    }

    auto end = (const char*) (message.data.data() + sizeof(OpenMessage));
    m_label.assign(end, openMessage.labelLength);
    m_protocol.assign(end + openMessage.labelLength, openMessage.protocolLength);

    // We don't parse reliability parameter: looks like all dataChannel implementations
    // always use TCP-like reliability.

    if (m_dataChannelOpened)
    {
        NX_DEBUG(
            this,
            "Data channel %1 is already open, but requested another channel %2"
            "to open from remote. Will read from both opened channels, "
            "but write only into first opened channel.",
            m_defaultStreamId,
            message.streamId);
    }
    else
    {
        m_defaultStreamId = message.streamId;
        m_dataChannelOpened = true;
    }

    NX_DEBUG(
        this,
        "Opened remote Data Channel with stream %1, label %2, protocol %3",
        message.streamId,
        m_label,
        m_protocol);

    uint8_t ack = MessageType::ack;
    const bool result = writeMessage(
        {Message::Type::control, message.streamId, std::string((const char*) &ack, 1), true});
    if (result && m_ice)
        m_ice->onDataChannelOpened(message.streamId);
    return result;
}

bool DataChannel::openDataChannel(const std::string& label, const std::string& protocol)
{
    if (m_dataChannelOpened)
        return true; //< Don't need another Data Channel.
    m_label = label;
    m_protocol = protocol;

    // RFC 8832 #6: the side acting as the DTLS server MUST open its channels
    // on an odd stream id (the client owns the even ones). This end always
    // takes the DTLS server role, and a strict peer - the toolbox's
    // libdatachannel - resets an even-stream open from the server as an
    // odd/even violation. Browsers accepted the old stream 0.
    m_defaultStreamId = 1;

    // Always use default reliability parameters: looks like all dataChannel implementations
    // always use TCP-like reliability.

    ChannelType channelType = ChannelType::reliable;
    uint32_t reliabilityParameter = 0;

    const size_t len = sizeof(OpenMessage) + m_label.size() + m_protocol.size();
    std::string buffer(len, 0);
    auto& openMessage = *(OpenMessage*) buffer.data();
    openMessage.type = MessageType::open;
    openMessage.channelType = channelType;
    openMessage.priority = htons(0);
    openMessage.reliabilityParameter = htonl(reliabilityParameter);
    openMessage.labelLength = htons((uint16_t) m_label.size());
    openMessage.protocolLength = htons((uint16_t) m_protocol.size());

    const auto end = buffer.data() + sizeof(OpenMessage);
    std::copy(m_label.begin(), m_label.end(), end);
    std::copy(m_protocol.begin(), m_protocol.end(), end + m_label.size());

    return writeMessage({Message::Type::control, m_defaultStreamId, buffer, true});
}

} // namespace nx::webrtc
