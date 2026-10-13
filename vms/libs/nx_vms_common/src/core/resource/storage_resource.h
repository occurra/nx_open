// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <core/resource/abstract_storage_resource.h>
#include <nx/utils/crypt/encryptable.h>

class QnAbstractMediaStreamDataProvider;

class NX_VMS_COMMON_API QnStorageResource:
    public QnAbstractStorageResource,
    public nx::crypt::Encryptable
{
    using base_type = QnAbstractStorageResource;

public:
    virtual ~QnStorageResource();

    void setSpaceLimit(qint64 value);
    virtual qint64 getSpaceLimit() const;

    void setStorageType(const QString& type);
    QString getStorageType() const;

    void setMaxStoreTime(int timeInSeconds);
    int getMaxStoreTime() const;

    void setUsedForWriting(bool isUsedForWriting);
    bool isUsedForWriting() const;

    static QString urlToPath(const QString& url);
    static QString urlWithoutCredentials(const QString& url);
    QString urlWithoutCredentials() const;

    bool isExternal() const;
    virtual bool isSystem() const { return false; }

    virtual qint64 bitrateBps() const;

    void addBitrate(QnAbstractMediaStreamDataProvider* provider);
    void releaseBitrate(QnAbstractMediaStreamDataProvider* provider);

    /*
     * Short and unique storage ID. It is addition related ID field, and used for memory usage
     * optimization
     */
    void setUrl(const QString& value);
    virtual QString getUrl() const;

    static QString toNativeDirPath(const QString& dirPath);

    void setBackup(bool value);
    bool isBackup() const;

    bool isWritable() const;
    bool isDbReady() const;

    virtual qint64 nxOccupiedSpace() const { return 0; }

    virtual QIODevice* open(const QString& fileName, QIODevice::OpenMode openMode) override;

protected:
    virtual QIODevice* openInternal(const QString& fileName, QIODevice::OpenMode openMode) = 0;

private:
    qint64 m_spaceLimit = QnAbstractStorageResource::kUnknownSize;
    int m_maxStoreTime = 0; // in seconds
    bool m_usedForWriting = false;
    std::atomic<float> m_storageBitrateCoeff;
    QString m_storageType;
    QSet<QnAbstractMediaStreamDataProvider*> m_providers;
    mutable nx::Mutex m_bitrateMtx;
    bool m_isBackup = false;
    QString m_url;
};
