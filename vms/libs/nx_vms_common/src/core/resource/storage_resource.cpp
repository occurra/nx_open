// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "storage_resource.h"

#include <nx/reflect/to_string.h>
#include <nx/streaming/abstract_media_stream_data_provider.h>
#include <nx/utils/log/log.h>

QnStorageResource::~QnStorageResource()
{
}

void QnStorageResource::setSpaceLimit(qint64 value)
{
    NX_MUTEX_LOCKER lock(&m_mutex);
    m_spaceLimit = value;
}

qint64 QnStorageResource::getSpaceLimit() const
{
    NX_MUTEX_LOCKER lock(&m_mutex);
    return m_spaceLimit;
}

void QnStorageResource::setStorageType(const QString& type)
{
    NX_MUTEX_LOCKER lock(&m_mutex);
    m_storageType = type;
}

QString QnStorageResource::getStorageType() const
{
    NX_MUTEX_LOCKER lock(&m_mutex);
    return m_storageType;
}

void QnStorageResource::setMaxStoreTime(int timeInSeconds)
{
    NX_MUTEX_LOCKER lock(&m_mutex);
    m_maxStoreTime = timeInSeconds;
}

int QnStorageResource::getMaxStoreTime() const
{
    NX_MUTEX_LOCKER lock(&m_mutex);
    return m_maxStoreTime;
}

void QnStorageResource::setUsedForWriting(bool isUsedForWriting)
{
    NX_MUTEX_LOCKER lock(&m_mutex);
    m_usedForWriting = isUsedForWriting;
}

bool QnStorageResource::isUsedForWriting() const
{
    NX_MUTEX_LOCKER lock(&m_mutex);
    return m_usedForWriting;
}

qint64 QnStorageResource::bitrateBps() const
{
    float rez = 0;
    NX_MUTEX_LOCKER lock(&m_bitrateMtx);
    for (const QnAbstractMediaStreamDataProvider* provider: m_providers)
        rez += provider->bitrateBitsPerSecond();
    return rez;
}

void QnStorageResource::addBitrate(QnAbstractMediaStreamDataProvider* provider)
{
    NX_MUTEX_LOCKER lock(&m_bitrateMtx);
    m_providers << provider;
}

void QnStorageResource::releaseBitrate(QnAbstractMediaStreamDataProvider* provider)
{
    NX_MUTEX_LOCKER lock(&m_bitrateMtx);
    m_providers.remove(provider);
}

QString QnStorageResource::urlToPath(const QString& url)
{
    if (!url.contains("://"))
        return url;
    else
        return QUrl(url).path();
}

QString QnStorageResource::urlWithoutCredentials() const
{
    return urlWithoutCredentials(getUrl());
}

QString QnStorageResource::urlWithoutCredentials(const QString& url)
{
    if (!url.contains("://"))
        return url;

    QUrl result(url);
    result.setUserName(QString());
    result.setPassword(QString());

    return result.toString();
}

void QnStorageResource::setUrl(const QString& value)
{
    NX_MUTEX_LOCKER lock(&m_mutex);
    m_url = value;
}

QString QnStorageResource::getUrl() const
{
    NX_MUTEX_LOCKER lock(&m_mutex);
    return m_url;
}

bool QnStorageResource::isExternal() const
{
    QString storageUrl = getUrl();
    return storageUrl.trimmed().startsWith("\\\\")
        || QUrl(storageUrl).path().mid(1).startsWith("\\\\") || storageUrl.indexOf("://") != -1;
}

QString QnStorageResource::toNativeDirPath(const QString& dirPath)
{
    QString result = QDir::toNativeSeparators(dirPath);
    if (!result.endsWith(QDir::separator()))
        result.append(QDir::separator());
    return result;
}

void QnStorageResource::setBackup(bool value)
{
    NX_MUTEX_LOCKER lk(&m_mutex);
    m_isBackup = value;
}

bool QnStorageResource::isBackup() const
{
    NX_MUTEX_LOCKER lk(&m_mutex);
    return m_isBackup;
}

bool QnStorageResource::isWritable() const
{
    return getCapabilities() & QnAbstractStorageResource::cap::WriteFile;
}

bool QnStorageResource::isDbReady() const
{
    return getCapabilities() & QnAbstractStorageResource::cap::DBReady;
}

QIODevice* QnStorageResource::open(const QString& fileName, QIODevice::OpenMode openMode)
{
    return openInternal(fileName, openMode);
}
