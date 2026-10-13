// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "webrtc_consumer.h"

#include <nx/media/meta_data_packet.h>
#include <nx/utils/log/log.h>

#include "webrtc_session.h"
#include "webrtc_streamer.h"
#include "webrtc_tracker.h"

namespace nx::webrtc {

const int kMediaQueueSize = 512;

//< If the peer doesn't answer the codec-change offer within this window, give up
//< the seamless switch and fall back to a full reconnect.
static constexpr auto kRenegotiationTimeout = std::chrono::seconds(5);

Consumer::Consumer(Session* session):
    m_mediaQueue(kMediaQueueSize),
    m_session(session),
    m_renegotiationTimer(session->aioThread())
{
    //< The one and only binding. It happens here, while the consumer is still
    //< unreachable, because a pollable may not be moved once anything can post
    //< to it: bindToAioThread swaps the owning thread but leaves already-queued
    //< posted calls on the old one, which then trips the isInSelfAioThread
    //< assert in AioTaskQueue::processPostedCalls. Providers, the tracker and
    //< the ices all post here, from their own threads, at times this class does
    //< not control - so there is no later moment when a move would be safe.
    m_pollable.bindToAioThread(session->aioThread());
}

Consumer::~Consumer()
{
    std::promise<void> stoppedPromise;
    m_pollable.dispatch(
        [this, &stoppedPromise]()
        {
            stopUnsafe();
            stoppedPromise.set_value();
        });
    stoppedPromise.get_future().wait();
}

bool Consumer::canAcceptData() const
{
    return m_mediaQueue.size() < kMediaQueueSize;
}

bool Consumer::startStream(Streamer* streamer)
{
    //< No rebinding here - see the constructor. The ice that calls us runs on
    //< this consumer's thread already: the session pins the ices it creates to
    //< it, and IceTcp moves itself there as soon as the first STUN names the
    //< session, which is well before the dtls handshake gets this far.
    m_streamer = streamer;
    return true;
}

void Consumer::stopUnsafe()
{
    NX_ASSERT(m_pollable.isInSelfAioThread());

    if (m_streamer)
        m_streamer->stop();

    m_needStop = true;
    m_renegotiationTimer.pleaseStopSync();
    m_pollable.pleaseStopSync();
}

void Consumer::startProvidersIfNeeded()
{
    if (!NX_ASSERT(m_session))
        return;
    if (!m_streamer)
        return; //< It is not started yet.
    runInMuxingThread(
        [this]()
        {
            for (auto& provider: m_session->allProviders())
            {
                if (!provider->isStarted())
                    provider->play(provider->getPosition(), false);
            }
        });
}

void Consumer::seek(
    nx::Uuid deviceId,
    int64_t timestampUs,
    bool keyframeOnly,
    StatusHandler handler,
    std::optional<RtpRebase> rebase)
{
    if (!NX_ASSERT(m_session))
        return;

    runInMuxingThread(
        [this, handler, deviceId, timestampUs, keyframeOnly, rebase]()
        {
            NX_VERBOSE(
                this,
                "SEEKPROF muxing-thread pickup target=%1us seq=%2",
                timestampUs,
                rebase ? rebase->seq : 0);
            auto doSeek = [this,
                           deviceId,
                           keyframeOnly,
                           rebase](int64_t timestampUs, auto provider, StatusHandler handler)
            {
                // The marker travels into the seek itself (bound to the jump
                // by the provider), so a source reading on another thread
                // cannot pair it with a different seek. The muxer rebase is
                // registered separately - it is matched later by opaque value.
                std::optional<int> marker;
                if (provider && rebase && rebase->seq != 0)
                {
                    marker = rebase->seq;
                    if (rebase->rtpBase90k >= 0)
                    {
                        m_session->muxer()->setPendingRtpRebase(
                            provider->deviceId(),
                            rebase->seq,
                            rebase->rtpBase90k);
                    }
                }
                auto status = provider && provider->seek(timestampUs, keyframeOnly, marker)
                    ? StreamStatus::success
                    : StreamStatus::nodata;

                m_pollable.dispatch(
                    [deviceId = provider ? provider->deviceId() : deviceId, status, handler]()
                    {
                        handler(deviceId, status);
                    });
            };

            if (!deviceId.isNull())
            {
                auto provider = m_session->provider(deviceId);
                doSeek(timestampUs, provider, handler);
            }
            else
            {
                for (const auto& provider: m_session->allProviders())
                    doSeek(timestampUs, provider, handler);
            }
        });
}

void Consumer::changeQuality(
    nx::Uuid deviceId,
    nx::vms::api::StreamIndex stream,
    StatusHandler handler)
{
    if (!NX_ASSERT(m_session))
        return;

    runInMuxingThread(
        [this, handler, deviceId, stream]()
        {
            auto doChangeQuality = [this, deviceId](auto provider, auto stream, auto handler)
            {
                auto status = provider && provider->changeQuality(stream) ? StreamStatus::success
                                                                          : StreamStatus::nodata;
                m_pollable.dispatch(
                    [deviceId = provider ? provider->deviceId() : deviceId, status, handler]()
                    {
                        handler(deviceId, status);
                    });
            };

            if (!deviceId.isNull())
            {
                auto provider = m_session->provider(deviceId);
                doChangeQuality(provider, stream, handler);
            }
            else
            {
                for (const auto& provider: m_session->allProviders())
                    doChangeQuality(provider, stream, handler);
            }
        });
}

void Consumer::pause(nx::Uuid deviceId, StatusHandler handler)
{
    if (!NX_ASSERT(m_session))
        return;

    runInMuxingThread(
        [this, handler = std::move(handler), deviceId]()
        {
            auto doPause = [deviceId](auto provider, auto handler)
            {
                auto status =
                    provider && provider->pause() ? StreamStatus::success : StreamStatus::nodata;
                handler(provider ? provider->deviceId() : deviceId, status);
            };

            if (!deviceId.isNull())
            {
                auto provider = m_session->provider(deviceId);
                doPause(provider, handler);
            }
            else
            {
                for (const auto& provider: m_session->allProviders())
                    doPause(provider, handler);
            }
        });
}

void Consumer::resume(
    nx::Uuid deviceId,
    std::optional<int64_t> positionUs,
    StatusHandler handler,
    std::optional<RtpRebase> rebase)
{
    if (!NX_ASSERT(m_session))
        return;

    runInMuxingThread(
        [this, handler = std::move(handler), deviceId, positionUs, rebase]()
        {
            auto doResume = [this, deviceId, positionUs, rebase](auto provider, auto handler)
            {
                // The repositioning inside the resume is a seek in all but name: the
                // marker and the muxer rebase follow the same contract as seek().
                std::optional<int> marker;
                if (provider && positionUs && rebase && rebase->seq != 0)
                {
                    marker = rebase->seq;
                    if (rebase->rtpBase90k >= 0)
                    {
                        m_session->muxer()->setPendingRtpRebase(
                            provider->deviceId(),
                            rebase->seq,
                            rebase->rtpBase90k);
                    }
                }
                auto status = provider && provider->resume(positionUs, marker)
                    ? StreamStatus::success
                    : StreamStatus::nodata;
                handler(provider ? provider->deviceId() : deviceId, status);
            };

            if (!deviceId.isNull())
            {
                auto provider = m_session->provider(deviceId);
                doResume(provider, handler);
            }
            else
            {
                for (const auto& provider: m_session->allProviders())
                    doResume(provider, handler);
            }
        });
}

void Consumer::nextFrame(
    nx::Uuid deviceId,
    int seq,
    std::optional<int64_t> positionUs,
    StatusHandler handler)
{
    if (!NX_ASSERT(m_session))
        return;

    runInMuxingThread(
        [this, handler, deviceId, seq, positionUs]()
        {
            m_stepSeq = seq;
            auto provider = m_session->provider(deviceId);
            auto status = provider && provider->nextFrame(positionUs) ? StreamStatus::success
                                                                      : StreamStatus::nodata;
            handler(deviceId, status);
        });
}

void Consumer::previousFrame(
    nx::Uuid deviceId,
    int seq,
    std::optional<int64_t> positionUs,
    StatusHandler handler)
{
    if (!NX_ASSERT(m_session))
        return;

    runInMuxingThread(
        [this, handler, deviceId, seq, positionUs]()
        {
            m_stepSeq = seq;
            auto provider = m_session->provider(deviceId);
            auto status = provider && provider->previousFrame(positionUs) ? StreamStatus::success
                                                                          : StreamStatus::nodata;
            handler(deviceId, status);
        });
}

void Consumer::reportPosition(const std::vector<CameraPosition>& positions)
{
    // The providers call this from their own threads; the streamer's channel write and
    // m_stepSeq live on the muxing thread.
    runInMuxingThread(
        [this, positions]()
        {
            if (m_stepSeq == 0 || !m_streamer)
                return;
            m_streamer->sendPosition(m_stepSeq, positions);
        });
}

bool Consumer::handleSrtp(std::vector<uint8_t> buffer)
{
    runInMuxingThread(
        [this, buffer = std::move(buffer)]() mutable
        {
            using namespace nx::rtp;
            if (buffer.size() < kRtcpHeaderSize)
            {
                NX_DEBUG(this, "Got invalid RTP packet with size %1", buffer.size());
                return;
            }
            auto rtpHeader = (RtpHeader*) (buffer.data());
            if (rtpHeader->isRtcp())
            {
                handleRtcp(buffer.data(), buffer.size());
            }
            else
            {
                NX_VERBOSE(this, "Got unexpected RTP packet from peer");
                // TODO Handle incoming RTP packet.
            }
        });

    return true;
}

void Consumer::handleRtcp(uint8_t* data, int size)
{
    // Should decrypt packet first, due to 'ssrc' field is in encrypted part of packet.
    auto encryptor = m_session->muxer()->getEncryptor();
    if (encryptor)
        encryptor->decryptPacket(data, &size);

    // Several RTCP packets can be glued into one lower level packet.
    while (size >= nx::rtp::kRtcpHeaderSize)
    {
        uint16_t length;
        memcpy(&length, data + 2, sizeof(length));
        length = ntohs(length);
        // length of the RTCP packet in 32-bit words minus one,
        // including the header and any padding.
        int packetLength = std::min((length + 1) * 4, size);
        uint32_t ssrc = nx::rtp::getRtcpSsrc(data, size);
        auto encoder = m_session->muxer()->encoderBySsrc(ssrc);
        if (encoder)
        {
            encoder->setRtcpPacket(data, packetLength);
            nx::utils::ByteArray buffer;
            while (encoder->getNextNackPacket(buffer))
            {
                m_streamer->ice()->writePacket(buffer.data(), buffer.size(), /*foreground*/ false);
                buffer.clear();
            }
        }
        else
        {
            // Some RTCP feedbacks should be demuxed by payloadType.
        }
        size -= packetLength;
        data += packetLength;
    }
}

void Consumer::onDataSent(bool success)
{
    // Should be posted to avoid deadlock due to recursion.
    if (success)
        m_pollable.post(
            [this]()
            {
                m_sendInProgress = false;
                processNextData();
            });
    else
        m_streamer->ice()->stopUnsafe();
}

void Consumer::sendMetadata(const QnConstCompressedMetadataPtr& metadata)
{
    // The payload is a JSON-serialized occurra::ai::MetadataPacket.
    // Wrap it in {"metadata": <packet>} and send via data channel.
    std::string json = "{\"metadata\":";
    json.append(metadata->data(), metadata->dataSize());
    json.append("}");
    // Unreliable transport: overlay metadata is keyed by its own timestamp and
    // superseded ~30 times a second, so on a lossy path a lost box may simply
    // vanish - retransmitting it would head-of-line block the messages behind
    // it past their display windows.
    m_streamer->ice()->writeDataChannelString(
        json,
        metadata->deviceId,
        /*unreliableTransport*/ true);
}

void Consumer::processNextData()
{
    //< Paused mid-codec-switch: keep buffering incoming packets in m_mediaQueue,
    //< but don't feed the encoder until the renegotiation is answered.
    if (m_renegotiating)
        return;

    while (!m_needStop)
    {
        auto media = m_mediaQueue.popData(false /*blocked*/);
        if (!media)
            break;

        NX_VERBOSE(this, "Process data: %1", media);

        // Send metadata via datachannel. The metadata subscription gates only
        // the realtime overlay types (object boxes, heat maps); notification
        // metadata (events, best shots, ...) is always delivered. The
        // subscription is per device in multi-camera sessions (a single-camera
        // session just uses the session default). Metadata never goes to the
        // RTP muxer either way.
        const auto metadata = std::dynamic_pointer_cast<const QnCompressedMetadata>(media);
        if (metadata)
        {
            if (!isOverlayMetadata(metadata) || isMetadataEnabledFor(metadata->deviceId))
                sendMetadata(metadata);
            continue;
        }

        if (media->dataType == QnAbstractMediaData::EMPTY_DATA)
        {
            // The device's archive ended while the session plays on. There is no RTP
            // payload to mux - the marker travels to the player as a stream status.
            m_streamer->handleStreamStatus(media->deviceId, StreamStatus::endOfArchive);
            continue;
        }

        const auto deviceId = media->deviceId;

        // A camera refused at this codec has no encoder: its media goes nowhere. A video packet
        // at another codec (an archive that changes codec mid-playback) lifts the refusal and
        // takes the renegotiation every codec change takes.
        if (const auto refused = m_session->unsupportedVideoCodec(deviceId))
        {
            if (media->dataType != QnAbstractMediaData::VIDEO || media->compressionType == *refused
                || !media->getContext())
            {
                continue;
            }
            m_session->forgetUnsupportedVideoCodec(deviceId);
            requestCodecRenegotiation(media);
            return;
        }

        // A packet whose device has left the session (a rapid retarget dropped it) must be
        // discarded, never held: holding it at the queue front would block every other camera's
        // media behind a device that will never drain, wedging the whole session.
        if (!m_session->provider(deviceId))
            continue;

        // The peer does not decode a just-activated m-line until it applies the switch answer,
        // so a keyframe sent before then is dropped - the primed GOP above all. Hold this
        // device's media at the queue front until its offer is answered, then it drains in order.
        if (m_session->offerInProgress(deviceId))
        {
            m_mediaQueue.pushFront(media);
            return;
        }

        auto rtpEncoder = m_session->muxer()->setDataPacket(deviceId, media);
        if (!rtpEncoder)
        {
            // A device that joined without this stream (an offline camera negotiates no
            // tracks) has no encoder to reject the packet, so the codec-change detection
            // below can never fire for it. The packet itself proves the stream exists
            // now - and carries the codec parameters - so mint its track through the
            // same renegotiation every codec change takes.
            if ((media->dataType == QnAbstractMediaData::VIDEO
                 || media->dataType == QnAbstractMediaData::AUDIO)
                && media->getContext() && !m_session->hasTrackForMedia(media))
            {
                requestCodecRenegotiation(media);
                return;
            }
            NX_VERBOSE(this, "Skip data: %1", media);
            return;
        }

        nx::utils::ByteArray buffer;
        std::deque<nx::Buffer> mediaBuffers;
        while (!m_needStop && m_session->muxer()->getNextPacket(rtpEncoder, buffer))
        {
            mediaBuffers.emplace_back(buffer.data(), buffer.size());
            buffer.clear();
        }

        // The source codec changed (e.g. a seek landed in a segment encoded with
        // a different codec): the encoder rejects the packet with EOF without
        // consuming it (rtp_universal_encoder.cpp). Rather than dropping the
        // stream, hold the untouched packet, rebuild the encoder for the new
        // codec and renegotiate the SDP. Once the peer answers, the held packet
        // is re-fed to the new encoder — a seamless, lossless switch.
        // NOTE: some high h.264 profiles can also be unsupported by the browser;
        // the transcoding fallback in processSdpAnswer() handles that case too.
        if (m_session->muxer()->isEof(rtpEncoder))
        {
            //< An audio codec the peer cannot receive can be transcoded into the
            //< one it already negotiated, which keeps the SDP - and therefore the
            //< peer - untouched. Only this packet is lost: it is already past the
            //< transcoder's insertion point.
            if (m_session->adaptToAudioCodecChange(deviceId, media))
                continue;

            requestCodecRenegotiation(media);
            return;
        }

        if (media->dataType == QnAbstractMediaData::VIDEO
            || media->dataType == QnAbstractMediaData::AUDIO)
        {
            // Force an immediate RTP→NTP timestamp report (bypassing the
            // m_sendTimestampInterval throttle) in two cases:
            //   - BOF: first packet after a seek (set by QnArchiveStreamReader).
            //   - PreRoll: every pre-roll frame from the seek I-frame up to and
            //     including the seek target (the last pre-roll frame - the first
            //     frame at or after the requested time). The player needs the
            //     RTP→UTC mapping for each so it can hide them until the
            //     requested UTC and reveal exactly at the target.
            const bool isPreRoll = media->flags.testFlag(QnAbstractMediaData::MediaFlags_PreRoll);
            const bool isBof = media->flags.testFlag(QnAbstractMediaData::MediaFlags_BOF);

            if (isBof || isPreRoll)
                m_lastReportedTimestampUs.erase(deviceId);

            auto timestamps = m_session->muxer()->getLastTimestamps(deviceId);
            const auto lastReported = m_lastReportedTimestampUs.find(deviceId);
            if (lastReported == m_lastReportedTimestampUs.end()
                || std::abs(timestamps.ntpTimestamp - lastReported->second)
                    > m_sendTimestampInterval.count() * 1000)
            {
                m_lastReportedTimestampUs[deviceId] = timestamps.ntpTimestamp;
                m_streamer->sendTimestamp(
                    deviceId,
                    timestamps.ntpTimestamp,
                    timestamps.rtpTimestamp);
            }
        }

        if (!mediaBuffers.empty())
        {
            // If the ice send buffer is empty, we send the data and start collecting the second
            // chunk. If the ice send buffer is busy, we stop collecting the data and wait for the
            // send with the ready chunk, it will be sent later in the callback onDataSent, so
            // sending is not wait for chunk collection.
            m_streamer->ice()->writeBatch(std::move(mediaBuffers));
            m_sendInProgress = true;
            break;
        }
    }
}

void Consumer::requestCodecRenegotiation(const QnConstAbstractMediaDataPtr& media)
{
    const auto deviceId = media->deviceId;

    auto tracker = m_session->tracker();
    if (!tracker || !tracker->isStarted())
    {
        //< No signaling channel to renegotiate over — fall back to the legacy
        //< full reconnect so the client can re-establish with the new codec.
        m_streamer->handleStreamStatus(deviceId, StreamStatus::reconnect);
        return;
    }

    NX_DEBUG(this, "Codec change for device %1; renegotiating SDP", deviceId);

    //< Seed the new codec parameters straight from the triggering packet so the
    //< encoder rebuild uses them verbatim, rather than re-probing the archive
    //< (which can read the previous segment's codec around a boundary). Seed the
    //< stream that actually changed — the EOF came from its encoder.
    if (const auto& context = media->getContext())
    {
        if (auto provider = m_session->provider(deviceId))
        {
            if (media->dataType == QnAbstractMediaData::VIDEO)
                provider->setVideoCodecParameters(context);
            else if (media->dataType == QnAbstractMediaData::AUDIO)
                provider->setAudioCodecParameters(context);
        }
    }

    //< Put the rejected packet back at the front so it is the first thing re-fed
    //< to the rebuilt encoder on resume — no frame lost, order preserved. Only
    //< for a codec the peer receives natively, though: anything else gets a
    //< transcoder inserted, so the rebuilt encoder is built for the transcoder's
    //< output while this packet - already past the transcoder's insertion point -
    //< stays in the source codec. Re-feeding it would earn a second rejection and
    //< a second renegotiation, for a packet no rebuild can ever accept.
    const bool isPayloadable = media->dataType == QnAbstractMediaData::VIDEO
        ? m_session->muxer()->isVideoCodecSupported(media->compressionType)
        : m_session->muxer()->isAudioCodecSupported(media->compressionType);
    if (isPayloadable)
        m_mediaQueue.pushFront(media);
    m_renegotiating = true;

    //< Safety net: if the peer never answers, fall back to a full reconnect.
    m_renegotiationTimer.start(
        kRenegotiationTimeout,
        [this, deviceId]()
        {
            if (!m_renegotiating)
                return;
            NX_WARNING(
                this,
                "Codec renegotiation for device %1 timed out; reconnecting",
                deviceId);
            m_renegotiating = false;
            m_streamer->handleStreamStatus(deviceId, StreamStatus::reconnect);
        });

    tracker->renegotiateCodec(deviceId);
}

void Consumer::resumeAfterRenegotiation()
{
    m_pollable.dispatch(
        [this]()
        {
            if (!m_renegotiating)
                return; //< Not a codec-change answer (e.g. initial/add-provider).
            NX_DEBUG(this, "Codec renegotiation answered; resuming media");
            m_renegotiationTimer.cancelSync();
            m_renegotiating = false;
            processNextData(); //< Pops the re-queued trigger packet first, then drains.
        });
}

void Consumer::runInMuxingThread(std::function<void()>&& func)
{
    m_pollable.dispatch(
        [func = std::move(func)]()
        {
            func();
        });
}

void Consumer::putData(const QnConstAbstractMediaDataPtr& data)
{
    m_pollable.dispatch(
        [this, data]()
        {
            // BOF marks the first packet of a new playback position after a
            // seek. Drop this device's pre-seek packets still queued for muxing —
            // they belong to the previous timeline and would just delay the new
            // content reaching the encoder. Only this device's: the other cameras
            // of the session have their own post-seek key frames in the same
            // queue by now, and each of them flushes its own backlog on its own
            // BOF. processNextData() below will then pick up this BOF packet
            // first, and the throttle reset in the BOF branch there will trigger
            // an immediate sync timestamp send to the player.
            if (data && data->flags.testFlag(QnAbstractMediaData::MediaFlags_BOF))
            {
                m_mediaQueue.clearUnprocessedData(data->deviceId);
                // The device's overlay metadata already handed to the data
                // channel sits in its own output queue, which the flush above
                // never reaches - drop it there too, or it goes out ahead of
                // the post-seek stream and delays it by however deep that
                // backlog is; the metadata then reaches the client after its
                // display window and never draws.
                if (m_streamer)
                    m_streamer->ice()->clearDataChannelStrings(data->deviceId);
            }
            m_mediaQueue.putData(data);
            if (!m_sendInProgress)
                processNextData();
        });
}

void Consumer::setSendTimestampInterval(std::chrono::milliseconds interval)
{
    m_sendTimestampInterval = interval;
}

void Consumer::setEnableMetadata(bool enableMetadata)
{
    // Set the default synchronously: createProvider seeds a new provider from
    // isMetadataEnabled() on the session thread right after this call, so a
    // dispatched write could arrive too late and seed the provider metadata-off.
    m_enableMetadata = enableMetadata;
    // The overrides are muxing-thread-only state; clear them there (a dispatch
    // runs inline when already on that thread).
    runInMuxingThread(
        [this]()
        {
            m_metadataOverrides.clear();
        });
}

void Consumer::setEnableMetadata(nx::Uuid deviceId, bool enableMetadata)
{
    runInMuxingThread(
        [this, deviceId, enableMetadata]()
        {
            m_metadataOverrides[deviceId] = enableMetadata;
        });
}

bool Consumer::isMetadataEnabledFor(nx::Uuid deviceId) const
{
    const auto it = m_metadataOverrides.find(deviceId);
    return it != m_metadataOverrides.end() ? it->second : m_enableMetadata.load();
}

} // namespace nx::webrtc
