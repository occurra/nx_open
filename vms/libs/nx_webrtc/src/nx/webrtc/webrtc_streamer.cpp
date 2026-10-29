// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "webrtc_streamer.h"

#include <cerrno>
#include <cstdlib>
#include <optional>
#include <vector>

#include <QtCore/QByteArray>

#include <nx/metric/metrics_storage.h>
#include <nx/reflect/json.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "webrtc_session.h"
#include "webrtc_session_pool.h"

namespace nx::webrtc {

Streamer::Streamer(Session* session):
    m_session(session)
{
}

Streamer::~Streamer()
{
}

std::string Streamer::idForToStringFromPtr() const
{
    return m_session->id();
}

bool Streamer::start(Ice* ice, Dtls* dtls)
{
    NX_ASSERT(ice);
    m_ice = ice;
    m_iceType = m_ice->type();
    updateMetrics(1);

    m_session->muxer()->setSrtpEncryptionData(dtls->encryptionData());

    if (!m_session->consumer()->startStream(this))
        return false;

    for (auto& provider: m_session->allProviders())
    {
        m_session->consumer()->seek(
            provider->deviceId(),
            provider->getPosition(),
            /*keyframeOnly*/ false,
            [this, provider](nx::Uuid deviceId, Consumer::StreamStatus result)
            {
                handleStreamStatus(deviceId, result);
            }); //< No rebase: the initial start streams from the negotiated position.
    }
    return true;
}

void Streamer::stop()
{
    updateMetrics(-1);
    m_session->stopAllProviders();
    m_iceType = IceCandidate::Filter::None;
}

bool Streamer::onSrtp(std::vector<uint8_t> buffer)
{
    return m_session->consumer()->handleSrtp(std::move(buffer));
}

static nx::Uuid parseDeviceId(const rapidjson::Value& obj)
{
    if (!obj.IsObject())
        return {};

    auto it = obj.FindMember("deviceId");
    if (it == obj.MemberEnd() || !it->value.IsString())
        return {};

    return nx::Uuid::fromStringSafe(it->value.GetString());
}

/** A time member, milliseconds since epoch on the wire (a number or a numeric string),
 *  converted to microseconds. Empty when absent, negative or unparsable. */
static std::optional<int64_t> parseTimeUs(const rapidjson::Value& obj, const char* key)
{
    if (!obj.IsObject())
        return std::nullopt;

    const auto it = obj.FindMember(key);
    if (it == obj.MemberEnd())
        return std::nullopt;

    const auto& v = it->value;
    int64_t ms = -1;
    if (v.IsDouble())
        ms = static_cast<int64_t>(v.GetDouble());
    else if (v.IsInt64())
        ms = v.GetInt64();
    else if (v.IsInt())
        ms = v.GetInt();
    else if (v.IsString())
    {
        const char* str = v.GetString();
        if (!str || !*str)
            return std::nullopt;

        char* end = nullptr;
        errno = 0;
        ms = std::strtoll(str, &end, 10);
        if (end == str || errno == ERANGE)
            return std::nullopt;
    }
    if (ms < 0)
        return std::nullopt;
    return ms * 1000;
}

/**
 * Optional keyframe-only (rough) seek, carried by both the seek and the
 * setCamera commands: land at the nearest preceding key frame with no pre-roll.
 * Absent or false keeps the precise behavior, which pre-rolls from that key
 * frame to the requested time - the server compresses the timestamps of those
 * hidden frames so the requested one arrives sooner, and the client keeps them
 * behind its freeze.
 */
static bool parseKeyframeOnly(const rapidjson::Value& obj)
{
    if (!obj.IsObject())
        return false;

    const auto it = obj.FindMember("keyframe");
    return it != obj.MemberEnd() && it->value.IsBool() && it->value.GetBool();
}

/**
 * Optional client-chosen RTP-timeline rebase (see Consumer::RtpRebase), carried
 * by both the seek and the setCamera commands: {"seq": <non-zero int>,
 * "rtpBase": <90 kHz ticks>}. The first packet tagged with seq restarts the RTP
 * timeline at the base, so the player can discard everything that came before by
 * comparing frame RTP timestamps against the base it picked itself - no reply
 * round-trip needed. Absent, or a zero seq, is a legacy client that does not
 * gate its reveal, and the timeline then continues unbroken. A missing rtpBase
 * leaves the sentinel -1, which arms the marker without restarting the timeline.
 */
static std::optional<Consumer::RtpRebase> parseRtpRebase(const rapidjson::Value& obj)
{
    if (!obj.IsObject())
        return std::nullopt;

    const auto it = obj.FindMember("seq");
    if (it == obj.MemberEnd() || !it->value.IsInt() || it->value.GetInt() == 0)
        return std::nullopt;

    Consumer::RtpRebase rebase;
    rebase.seq = it->value.GetInt();
    rebase.rtpBase90k = -1;
    if (const auto rb = obj.FindMember("rtpBase"); rb != obj.MemberEnd())
    {
        if (rb->value.IsInt64())
            rebase.rtpBase90k = rb->value.GetInt64();
        else if (rb->value.IsDouble())
            rebase.rtpBase90k = static_cast<int64_t>(rb->value.GetDouble());
    }
    return rebase;
}

/** The client-chosen command sequence, 0 when absent (a legacy client). */
static int parseSeq(const rapidjson::Value& obj)
{
    const auto it = obj.FindMember("seq");
    return it != obj.MemberEnd() && it->value.IsInt() ? it->value.GetInt() : 0;
}

static nx::vms::api::StreamIndex parseStreamIndex(const rapidjson::Value& obj)
{
    auto it = obj.FindMember("stream");
    if (it == obj.MemberEnd())
        return nx::vms::api::StreamIndex::primary;

    const auto& v = it->value;
    if ((v.IsString() && std::string{v.GetString()} == "1") || (v.IsInt() && v.GetInt() == 1)
        || (v.IsInt64() && v.GetInt64() == 1))
    {
        return nx::vms::api::StreamIndex::secondary;
    }

    return nx::vms::api::StreamIndex::primary;
}

void Streamer::onDataChannelString(const std::string& data, int /*streamId*/)
{
    rapidjson::Document d;
    d.Parse(data.data());
    if (!d.IsObject())
        return;

    nx::Uuid deviceId = parseDeviceId(d);

    // Replace the camera this session streams, in place: the new camera
    // inherits the negotiated tracks and encoders, so a same-codec switch never
    // reaches the peer as an offer/answer (see Session::setCamera). Sent instead
    // of a remove + add pair, which renegotiates twice.
    //   {"setCamera": {"deviceId": <new>, "seek": ms, "stream": 0|1,
    //                  "keyframe": bool, "seq": n, "rtpBase": ticks}}
    //   {"setCamera": "<new deviceId>"}
    // keyframe and seq/rtpBase are the same options a seek carries: rough vs
    // precise landing, and the RTP rebase that lets the player hold the previous
    // camera's picture until a frame it can prove came from this switch arrives.
    // The camera being replaced is the one streaming, so the client never has
    // to name it. Multi-camera sessions keep using add/remove.
    if (auto it = d.FindMember("setCamera"); it != d.MemberEnd())
    {
        const auto& setObj = it->value;
        //< The camera to switch TO is the one named inside the command. It wins
        //< over a top-level deviceId, which on every other command addresses the
        //< device being acted on - here that would be the one being replaced.
        if (setObj.IsObject() && !parseDeviceId(setObj).isNull())
            deviceId = parseDeviceId(setObj);
        else if (setObj.IsString())
            deviceId = nx::Uuid::fromStringSafe(setObj.GetString());
        if (deviceId.isNull())
        {
            NX_WARNING(this, "setCamera needs the deviceId to switch to");
            return;
        }

        std::optional<std::chrono::milliseconds> positionMs;
        if (const auto seekUs = parseTimeUs(setObj, "seek"))
            positionMs = std::chrono::milliseconds(*seekUs / 1000);

        const auto stream =
            setObj.IsObject() ? parseStreamIndex(setObj) : nx::vms::api::StreamIndex::primary;

        try
        {
            if (m_session->setCamera(
                    deviceId,
                    positionMs,
                    stream,
                    parseKeyframeOnly(setObj),
                    parseRtpRebase(setObj)))
            {
                //< Acknowledge the switch. A client talking to a server that
                //< predates setCamera gets no reply and can fall back to
                //< rebuilding the session, rather than watching the previous
                //< camera forever.
                handleStreamStatus(
                    deviceId,
                    Consumer::StreamStatus::success,
                    setObj.IsObject() ? parseSeq(setObj) : 0);
            }
            else
            {
                NX_WARNING(this, "Can't switch to camera %1", deviceId);
            }
        }
        catch (const std::exception& e)
        {
            NX_WARNING(this, "Can't switch camera: %1", e.what());
        }
        return;
    }

    // Reconcile the whole camera set in one shot:
    //   {"setCameras": [{"deviceId": <id>, "stream": 0|1}, ...], "seek": ms}
    // The server diffs it against the streaming providers and sends a single offer, so a switch
    // is one command and one renegotiation instead of a fast add-then-remove that races two
    // rounds. The optional "seek" positions the cameras this command newly adds (archive), the
    // same key a seek carries.
    if (auto it = d.FindMember("setCameras"); it != d.MemberEnd())
    {
        const auto& arr = it->value;
        if (!arr.IsArray())
        {
            NX_WARNING(this, "setCameras needs an array of {deviceId, stream}");
            return;
        }

        std::optional<std::chrono::milliseconds> positionMs;
        if (const auto seekUs = parseTimeUs(d, "seek"))
            positionMs = std::chrono::milliseconds(*seekUs / 1000);

        std::vector<Session::CameraRequest> cameras;
        for (const auto& item: arr.GetArray())
        {
            if (!item.IsObject())
                continue;
            const auto camera = parseDeviceId(item);
            if (camera.isNull())
                continue;
            cameras.push_back({camera, parseStreamIndex(item)});
        }
        if (cameras.empty())
        {
            NX_WARNING(this, "setCameras: no valid camera in the list");
            return;
        }

        // An in-place retarget sends no offer, so - like setCamera - acknowledge each switched-in
        // camera directly, letting the client bind that m-line to the new device. Extra
        // arrivals/departures ride the single offer setCameras sends and need no ack here.
        // seq/rtpBase ride at the top level, the same rebase a seek carries, applied to every
        // retargeted camera so the player can discard the previous camera's frames by RTP.
        for (const auto& deviceId: m_session->setCameras(cameras, positionMs, parseRtpRebase(d)))
            handleStreamStatus(deviceId, Consumer::StreamStatus::success, parseSeq(d));
        return;
    }

    if (auto it = d.FindMember("add"); it != d.MemberEnd())
    {
        const auto& addObj = it->value;
        if (!addObj.IsObject())
            return;

        if (deviceId.isNull())
            deviceId = parseDeviceId(addObj);
        if (deviceId.isNull())
            return;

        std::optional<std::chrono::milliseconds> positionMs;
        if (const auto seekUs = parseTimeUs(addObj, "seek"))
            positionMs = std::chrono::milliseconds(*seekUs / 1000);

        const nx::vms::api::StreamIndex newStream = parseStreamIndex(addObj);

        try
        {
            m_session->createProvider(deviceId, positionMs, newStream, std::nullopt /*speedOpt*/);
        }
        catch (const std::exception& e)
        {
            NX_WARNING(this, "Can't create provider: %1", e.what());
        }
        return;
    }

    if (d.HasMember("remove"))
    {
        if (deviceId.isNull())
        {
            if (const auto& value = d["remove"]; value.IsString())
                deviceId = nx::Uuid::fromStringSafe(value.GetString());
        }
        if (!deviceId.isNull())
            m_session->removeProvider(deviceId);
        else
            NX_WARNING(this, "deviceId should be provided to remove stream");
    }
    else if (d.HasMember("seek"))
    {
        const auto seekUs = parseTimeUs(d, "seek");
        const bool keyframeOnly = parseKeyframeOnly(d);
        const auto rebase = parseRtpRebase(d);

        // Absent or negative means go to live.
        const int64_t timestampUs = seekUs.value_or(DATETIME_NOW);
        NX_VERBOSE(
            this,
            "SEEKPROF recv target=%1ms seq=%2",
            seekUs ? *seekUs / 1000 : -1,
            rebase ? rebase->seq : 0);
        m_session->consumer()->seek(
            deviceId,
            timestampUs,
            keyframeOnly,
            [this, seq = parseSeq(d)](nx::Uuid deviceId, Consumer::StreamStatus result)
            {
                handleStreamStatus(deviceId, result, seq);
            },
            rebase);
    }
    else if (d.HasMember("stream"))
    {
        nx::vms::api::StreamIndex newStream = nx::vms::api::StreamIndex::primary;
        const auto& streamValue = d["stream"];
        if ((streamValue.IsString() && std::string{streamValue.GetString()} == "1")
            || (streamValue.IsInt() && streamValue.GetInt() == 1))
            newStream = nx::vms::api::StreamIndex::secondary;

        m_session->consumer()->changeQuality(
            deviceId,
            newStream,
            [this, seq = parseSeq(d)](nx::Uuid deviceId, Consumer::StreamStatus result)
            {
                handleStreamStatus(deviceId, result, seq);
            });
    }
    else if (d.HasMember("pause"))
    {
        if (deviceId.isNull())
        {
            const auto& pauseValue = d["pause"];
            if (!pauseValue.IsString())
            {
                NX_DEBUG(this, "Invalid pause method: %1", data);
                return;
            }
            deviceId = nx::Uuid::fromStringSafe(pauseValue.GetString());
        }
        m_session->consumer()->pause(
            deviceId,
            [this, seq = parseSeq(d)](nx::Uuid deviceId, Consumer::StreamStatus result)
            {
                handleStreamStatus(deviceId, result, seq);
            });
    }
    else if (d.HasMember("resume"))
    {
        if (deviceId.isNull())
            deviceId = nx::Uuid::fromStringSafe(d["resume"].GetString());
        m_session->consumer()->resume(
            deviceId,
            parseTimeUs(d, "position"),
            [this, seq = parseSeq(d)](nx::Uuid deviceId, Consumer::StreamStatus result)
            {
                handleStreamStatus(deviceId, result, seq);
            },
            parseRtpRebase(d));
    }
    else if (d.HasMember("nextFrame"))
    {
        // {"nextFrame": "<deviceId>", "seq": n}: seq is echoed by the position report
        // sent once the step settled (see Consumer::reportPosition); absent for a
        // legacy client, which then gets no report.
        if (deviceId.isNull())
            deviceId = nx::Uuid::fromStringSafe(d["nextFrame"].GetString());
        m_session->consumer()->nextFrame(
            deviceId,
            parseSeq(d),
            parseTimeUs(d, "position"),
            [this, seq = parseSeq(d)](nx::Uuid deviceId, Consumer::StreamStatus result)
            {
                handleStreamStatus(deviceId, result, seq);
            });
    }
    else if (d.HasMember("previousFrame"))
    {
        if (deviceId.isNull())
            deviceId = nx::Uuid::fromStringSafe(d["previousFrame"].GetString());
        m_session->consumer()->previousFrame(
            deviceId,
            parseSeq(d),
            parseTimeUs(d, "position"),
            [this, seq = parseSeq(d)](nx::Uuid deviceId, Consumer::StreamStatus result)
            {
                handleStreamStatus(deviceId, result, seq);
            });
    }
    else if (d.HasMember("metadata"))
    {
        // Overlay metadata subscription (object boxes, heat maps; notification
        // metadata is always delivered):
        //   {"metadata": bool}                       - the whole session
        //   {"metadata": bool, "deviceId": "<uuid>"} - one camera only
        // The scoped form serves multi-camera sessions (layouts), where tiles
        // subscribe independently. The session-wide form also clears every
        // per-device override, and providers added later inherit the session
        // default, not any override.
        const bool enable = d["metadata"].IsBool() ? d["metadata"].GetBool()
            : d["metadata"].IsInt()                ? d["metadata"].GetInt() != 0
                                                   : false;
        if (deviceId.isNull())
        {
            m_session->consumer()->setEnableMetadata(enable);
            for (const auto& p: m_session->allProviders())
                p->setEnableMetadata(enable);
        }
        else
        {
            m_session->consumer()->setEnableMetadata(deviceId, enable);
            if (const auto provider = m_session->provider(deviceId))
                provider->setEnableMetadata(enable);
            else
                NX_DEBUG(this, "No provider %1 to scope metadata to", deviceId);
        }
    }
    else if (
        auto talkIt = d.FindMember("talk"); talkIt != d.MemberEnd() && talkIt->value.IsObject())
    {
        // Talk-back on/off for a set of cameras, carried on this video session's data
        // channel instead of a separate connection:
        //   {"talk": {"deviceIds": ["<uuid>", ...], "on": bool}}
        //   {"talk": {"deviceId": "<uuid>", "on": bool}}  - legacy single-camera form
        // The Opus itself arrives as binary frames (see onDataChannelBinary).
        const auto& talk = talkIt->value;
        std::vector<nx::Uuid> talkDevices;
        if (const auto idsIt = talk.FindMember("deviceIds");
            idsIt != talk.MemberEnd() && idsIt->value.IsArray())
        {
            for (const auto& item: idsIt->value.GetArray())
            {
                if (!item.IsString())
                    continue;
                if (const auto id = nx::Uuid::fromStringSafe(item.GetString()); !id.isNull())
                    talkDevices.push_back(id);
            }
        }
        else if (const auto id = parseDeviceId(talk); !id.isNull())
        {
            talkDevices.push_back(id);
        }

        const bool on = talk.HasMember("on") && talk["on"].IsBool() && talk["on"].GetBool();
        if (!talkDevices.empty() && m_talkHandlers.onControl)
            m_talkHandlers.onControl(talkDevices, on);
    }
}

void Streamer::setTalkHandlers(TalkHandlers handlers)
{
    m_talkHandlers = std::move(handlers);
}

void Streamer::onDataChannelBinary(const std::string& data, int streamId)
{
    // Talk-back audio: a deviceId-prefixed binary frame - 16 raw UUID bytes then
    // the Opus packet. Only a session the media_server armed with talk handlers
    // reads binary this way; any other session keeps the legacy behaviour of
    // parsing a binary frame as a JSON command, so nothing that works today
    // regresses.
    constexpr size_t kDeviceIdBytes = 16;
    if (m_talkHandlers.onAudio && data.size() > kDeviceIdBytes)
    {
        const nx::Uuid deviceId =
            nx::Uuid::fromRfc4122(QByteArray(data.data(), (int) kDeviceIdBytes));
        if (!deviceId.isNull())
        {
            m_talkHandlers.onAudio(deviceId, data.substr(kDeviceIdBytes));
            return;
        }
    }
    onDataChannelString(data, streamId);
}

Ice* Streamer::ice()
{
    NX_ASSERT(m_ice);
    return m_ice;
}

void Streamer::sendTimestamp(nx::Uuid deviceId, int64_t timestampUs, uint32_t rtpTimestamp)
{
    rapidjson::Document message(rapidjson::kObjectType);
    // Simple (dash-less) form - the same key the client already uses for msid demux.
    // StringRef stores the pointer without copying; deviceIdStr outlives Accept() below.
    const std::string deviceIdStr = deviceId.toSimpleStdString();
    message.AddMember(
        "deviceId",
        rapidjson::StringRef(deviceIdStr.c_str()),
        message.GetAllocator());
    message.AddMember("timestampUs", timestampUs, message.GetAllocator());
    message.AddMember("rtpTimestamp", rtpTimestamp, message.GetAllocator());
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    message.Accept(writer);
    std::string data{buffer.GetString(), buffer.GetSize()};

    ice()->writeDataChannelString(data);
}

void Streamer::handleStreamStatus(nx::Uuid deviceId, Consumer::StreamStatus result, int seq)
{
    auto provider = m_session->provider(deviceId);
    if (!provider)
        return;

    rapidjson::Document message(rapidjson::kObjectType);
    auto positionMs = provider->isLiveMode() ? -1.0 : provider->getPosition() / 1000;

    if (result == Consumer::StreamStatus::reconnect) //< If reconnect send timestamp to reconnect.
        positionMs = m_session->muxer()->getLastTimestampMs();

    message.AddMember(
        "deviceId",
        provider->deviceId().toSimpleStdString(),
        message.GetAllocator());
    message.AddMember("timestampMs", positionMs, message.GetAllocator());
    message.AddMember("status", result, message.GetAllocator());
    if (seq != 0)
        message.AddMember("seq", seq, message.GetAllocator());
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    message.Accept(writer);
    NX_DEBUG(this, "Write stream status: %1", buffer.GetString());

    std::string data{buffer.GetString(), buffer.GetSize()};

    ice()->writeDataChannelString(data);
}

void Streamer::sendVideoTransport(nx::Uuid deviceId, bool isDataChannel)
{
    rapidjson::Document message(rapidjson::kObjectType);
    const std::string deviceIdStr = deviceId.toSimpleStdString();
    message.AddMember(
        "deviceId",
        rapidjson::StringRef(deviceIdStr.c_str()),
        message.GetAllocator());
    message.AddMember(
        "videoTransport",
        rapidjson::StringRef(isDataChannel ? "dataChannel" : "rtp"),
        message.GetAllocator());
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    message.Accept(writer);
    NX_DEBUG(this, "Write video transport: %1", buffer.GetString());

    ice()->writeDataChannelString(std::string(buffer.GetString(), buffer.GetSize()));
}

NX_REFLECTION_INSTRUMENT(CameraPosition, (deviceId)(timestampUs))

namespace {

/** Where paused playback settled after a frame step, per camera; the client moves its
 *  playhead from it and reveals each tile on its own settled frame (paused playback does
 *  not derive the position from presented frames). */
struct PositionMessage
{
    int seq = 0;
    std::vector<CameraPosition> positions;
};
NX_REFLECTION_INSTRUMENT(PositionMessage, (seq)(positions))

} // namespace

void Streamer::sendPosition(int seq, const std::vector<CameraPosition>& positions)
{
    ice()->writeDataChannelString(nx::reflect::json::serialize(PositionMessage{seq, positions}));
}

void Streamer::updateMetrics(int value)
{
    switch (m_iceType)
    {
        case IceCandidate::Filter::TcpHost:
            if (value > 0)
                m_session->metrics()->webrtc().totalTcpConnections() += value;
            m_session->metrics()->webrtc().activeTcpConnections() += value;
            break;
        case IceCandidate::Filter::UdpHost:
            if (value > 0)
                m_session->metrics()->webrtc().totalUdpConnections() += value;
            m_session->metrics()->webrtc().activeUdpConnections() += value;
            break;
        case IceCandidate::Filter::UdpSrflx:
            if (value > 0)
                m_session->metrics()->webrtc().totalSrflxConnections() += value;
            m_session->metrics()->webrtc().activeSrflxConnections() += value;
            break;
        case IceCandidate::Filter::UdpRelay:
            if (value > 0)
                m_session->metrics()->webrtc().totalRelayConnections() += value;
            m_session->metrics()->webrtc().activeRelayConnections() += value;
            break;
        case IceCandidate::Filter::None:
            break;
        default:
            NX_ASSERT(false, "Unexpected ice type: %1", m_iceType);
            break;
    }
}

} // namespace nx::webrtc
