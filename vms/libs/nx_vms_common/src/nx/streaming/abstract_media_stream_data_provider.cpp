// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "abstract_media_stream_data_provider.h"

#include <chrono>

#include <QtCore/QThread>

#include <core/resource/resource_media_layout.h>
#include <nx/media/config.h>
#include <nx/media/media_data_packet.h>
#include <nx/media/video_data_packet.h>
#include <nx/streaming/nx_streaming_ini.h>
#include <nx/utils/log/log.h>
#include <nx/utils/time_helper.h>
#include <utils/common/synctime.h>
#include <utils/common/util.h>

using namespace std::chrono_literals;

static const int kMaxErrorsToReportIssue = 2;

static std::chrono::microseconds sMediaStatisticsWindowSize = 2s;
static int sMediaStatisticsMaxDurationInFrames = 0;

void QnAbstractMediaStreamDataProvider::setMediaStatisticsWindowSize(
    std::chrono::microseconds value)
{
    sMediaStatisticsWindowSize = value;
}

void QnAbstractMediaStreamDataProvider::setMediaStatisticsMaxDurationInFrames(int value)
{
    sMediaStatisticsMaxDurationInFrames = value;
}

bool QnAbstractMediaStreamDataProvider::isConnectionLost() const
{
    return m_numberOfErrors >= kMaxErrorsToReportIssue;
}

void QnAbstractMediaStreamDataProvider::onEvent(CameraDiagnostics::Result event)
{
    if (event.errorCode == CameraDiagnostics::ErrorCode::noError)
        m_numberOfErrors = 0;
    else
        ++m_numberOfErrors;
}

QnAbstractMediaStreamDataProvider::QnAbstractMediaStreamDataProvider(const QString& url):
    m_url(url)
{
    m_gotKeyFrame.fill(0);
    resetTimeCheck();
}

QnAbstractMediaStreamDataProvider::~QnAbstractMediaStreamDataProvider()
{
    stop();
}

void QnAbstractMediaStreamDataProvider::setNeedKeyData(int channel)
{
    if (channel < CL_MAX_CHANNEL_NUMBER)
        m_gotKeyFrame.at(channel) = 0;
}

void QnAbstractMediaStreamDataProvider::setNeedKeyData()
{
    m_gotKeyFrame.fill(0);
}

bool QnAbstractMediaStreamDataProvider::needKeyData(int channel) const
{
    if (channel < CL_MAX_CHANNEL_NUMBER)
        return m_gotKeyFrame.at(channel) == 0;
    return false;
}

bool QnAbstractMediaStreamDataProvider::needKeyData() const
{
    for (int i = 0; i < getNumberOfChannels(); ++i)
    {
        if (m_gotKeyFrame.at(i) == 0)
            return true;
    }

    return false;
}

void QnAbstractMediaStreamDataProvider::beforeRun()
{
    setNeedKeyData();
    resetMediaStatistics();
    m_numberOfErrors = 0;
}

void QnAbstractMediaStreamDataProvider::resetMediaStatistics()
{
    for (int i = 0; i < CL_MAX_CHANNEL_NUMBER; ++i)
    {
        m_stat[i].setWindowSize(sMediaStatisticsWindowSize);
        m_stat[i].setMaxDurationInFrames(sMediaStatisticsMaxDurationInFrames);
        m_stat[i].reset();
    }
}

void QnAbstractMediaStreamDataProvider::afterRun()
{
}

const QnMediaStreamStatistics* QnAbstractMediaStreamDataProvider::getStatistics(int channel) const
{
    return &m_stat[channel];
}

int QnAbstractMediaStreamDataProvider::getNumberOfChannels() const
{
    return 1;
}

qint64 QnAbstractMediaStreamDataProvider::bitrateBitsPerSecond() const
{
    qint64 rez = 0;
    for (int i = 0; i < getNumberOfChannels(); ++i)
        rez += m_stat[i].bitrateBitsPerSecond();
    return rez;
}

float QnAbstractMediaStreamDataProvider::getFrameRate() const
{
    float rez = 0;
    for (int i = 0; i < getNumberOfChannels(); ++i)
        rez += m_stat[i].getFrameRate();

    return rez / getNumberOfChannels();
}

float QnAbstractMediaStreamDataProvider::getAverageGopSize() const
{
    float rez = 0;
    for (int i = 0; i < getNumberOfChannels(); ++i)
        rez += m_stat[i].getAverageGopSize();

    return rez / getNumberOfChannels();
}

void QnAbstractMediaStreamDataProvider::resetTimeCheck()
{
    for (int i = 0; i < CL_MAX_CHANNEL_NUMBER + 1; ++i)
        m_lastMediaTime[i] = AV_NOPTS_VALUE;
}

CameraDiagnostics::Result QnAbstractMediaStreamDataProvider::diagnoseMediaStreamConnection()
{
    return CameraDiagnostics::NotImplementedResult();
}
