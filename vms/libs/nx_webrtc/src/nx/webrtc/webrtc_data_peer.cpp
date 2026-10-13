// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "webrtc_data_peer.h"

#include <nx/utils/log/log.h>

#include "webrtc_session.h"

namespace nx::webrtc {

DataPeer::DataPeer(Session* session, nx::network::aio::AbstractAioThread* aioThread):
    m_session(session)
{
    //< Bound here and never rebound - see the aioThread parameter note.
    m_pollable.bindToAioThread(aioThread);
}

DataPeer::~DataPeer()
{
    m_pollable.pleaseStopSync();
}

void DataPeer::setHandlers(DataPeerHandlers handlers)
{
    m_handlers = std::move(handlers);
}

bool DataPeer::start(Ice* ice, Dtls* /*dtls*/)
{
    NX_ASSERT(ice);
    if (m_closed)
        return false;
    m_ice = ice;
    //< Not open yet: DCEP is still in flight. onDataChannelOpened() reports
    //< the usable channel.
    return true;
}

void DataPeer::stop()
{
    if (m_closed.exchange(true))
        return; //< Already closed; onClosed must fire at most once.
    m_open = false;
    NX_DEBUG(this, "%1: Data peer closed", m_session->id());
    if (m_handlers.onClosed)
        m_handlers.onClosed();
}

bool DataPeer::onSrtp(std::vector<uint8_t> /*buffer*/)
{
    return true; //< A data session negotiates no media; ignore stray SRTP.
}

void DataPeer::onDataChannelString(const std::string& /*data*/, int /*streamId*/)
{
    //< The protocol on top is binary-only; drop stray text messages.
}

void DataPeer::onDataChannelBinary(const std::string& data, int /*streamId*/)
{
    if (m_closed)
        return;
    if (m_handlers.onMessage)
        m_handlers.onMessage(data);
}

void DataPeer::onDataChannelOpened(int streamId)
{
    if (m_closed)
        return;
    NX_DEBUG(this, "%1: Data peer channel open, stream %2", m_session->id(), streamId);
    if (m_open.exchange(true))
        return; //< A peer may open a redundant second channel; report once.
    if (m_handlers.onOpen)
        m_handlers.onOpen();
}

void DataPeer::onDataChannelQueueProgress(int queueDepth)
{
    if (m_closed)
        return;
    if (m_handlers.onQueueProgress)
        m_handlers.onQueueProgress(queueDepth);
}

bool DataPeer::sendBinary(std::string data)
{
    if (m_closed || !m_ice.load())
        return false;
    //< Hop onto the session thread: there the closed check is ordered against
    //< Ice::stopUnsafe (which runs on that thread and marks us closed via
    //< stop()), so the write can never race a torn-down ice. Inline when
    //< already on the thread.
    m_pollable.dispatch(
        [this, data = std::move(data)]()
        {
            if (m_closed)
                return;
            if (Ice* const ice = m_ice.load())
                ice->writeDataChannelBinary(data);
        });
    return true;
}

int DataPeer::sendQueueDepth() const
{
    Ice* const ice = m_ice.load();
    return ice ? ice->dataChannelQueueSize() : 0;
}

bool DataPeer::isOpen() const
{
    return m_open && !m_closed;
}

} // namespace nx::webrtc
