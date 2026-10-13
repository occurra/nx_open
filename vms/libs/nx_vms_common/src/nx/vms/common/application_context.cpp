// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "application_context.h"

#include <QtCore/QCoreApplication>

#include <core/resource/storage_plugin_factory.h>
#include <nx/branding.h>
#include <nx/media/ffmpeg_helper.h>
#include <nx/metric/application_metrics_storage.h>
#include <nx/network/address_resolver.h>
#include <nx/network/socket_global.h>
#include <nx/utils/i18n/translation_manager.h>
#include <nx/utils/thread/long_runnable.h>
#include <nx/utils/thread/mutex.h>
#include <nx/utils/timer_manager.h>
#include <nx/vms/common/common_meta_types.h>
#include <utils/common/long_runable_cleanup.h>

// Resources initialization must be located outside of the namespace.
static void initializeResources()
{
    Q_INIT_RESOURCE(nx_vms_common);
}

namespace nx::vms::common {

using namespace nx::network;
using namespace nx::metric;

static ApplicationContext* s_instance = nullptr;

struct ApplicationContext::Private
{
    ApplicationContext* const q;
    const Features features;

    mutable nx::Mutex mutex;
    QString locale{nx::branding::defaultLocale()};

    std::unique_ptr<QnLongRunnablePool> longRunnablePool;
    std::unique_ptr<QnLongRunableCleanup> longRunableCleanup;
    std::unique_ptr<QnStoragePluginFactory> storagePluginFactory;
    std::unique_ptr<nx::utils::TimerManager> timerManager;
    std::unique_ptr<ApplicationMetricsStorage> metricsStorage;
    std::unique_ptr<nx::i18n::TranslationManager> translationManager;

    void initNetworking()
    {
        SocketGlobals::init();
        SocketGlobals::addressResolver().addFixedAddress(
            "localhost",
            SocketAddress::anyPrivateAddress);
    }

    void deinitNetworking()
    {
        SocketGlobals::addressResolver().removeFixedAddress(
            "localhost",
            SocketAddress::anyPrivateAddress);
        SocketGlobals::deinit();
    }
};

ApplicationContext::ApplicationContext(Features features, QObject* parent):
    QObject(parent),
    d(new Private{.q = this, .features = features})
{
    if (NX_ASSERT(!s_instance))
        s_instance = this;

    initializeResources();
    initializeMetaTypes();

    d->longRunnablePool = std::make_unique<QnLongRunnablePool>();
    d->longRunableCleanup = std::make_unique<QnLongRunableCleanup>();
    QnFfmpegHelper::registerLogCallback();

    if (features.flags.testFlag(FeatureFlag::networking))
        d->initNetworking();

    d->storagePluginFactory = std::make_unique<QnStoragePluginFactory>();
    d->timerManager = std::make_unique<nx::utils::TimerManager>("CommonTimerManager");
    d->metricsStorage = std::make_unique<ApplicationMetricsStorage>();

    if (features.flags.testFlag(FeatureFlag::translations))
        d->translationManager = std::make_unique<nx::i18n::TranslationManager>();
}

void ApplicationContext::stopAll()
{
    NX_VERBOSE(this, "Stop");

    d->longRunnablePool->stopAll();
}

ApplicationContext::~ApplicationContext()
{
    d->longRunnablePool->stopAll();

    // Long runnables with async destruction should be destroyed before we deinitialize networking.
    d->longRunableCleanup.reset();
    d->longRunnablePool.reset();

    if (d->features.flags.testFlag(FeatureFlag::networking))
        d->deinitNetworking();

    d->translationManager.reset();

    if (NX_ASSERT(s_instance == this))
        s_instance = nullptr;
}

ApplicationContext* ApplicationContext::instance()
{
    return s_instance;
}

QString ApplicationContext::locale() const
{
    return d->locale;
}

void ApplicationContext::setLocale(const QString& value)
{
    d->locale = value;
}

nx::i18n::TranslationManager* ApplicationContext::translationManager() const
{
    return d->translationManager.get();
}

QnStoragePluginFactory* ApplicationContext::storagePluginFactory() const
{
    return d->storagePluginFactory.get();
}

QnLongRunableCleanup* ApplicationContext::longRunableCleanup() const
{
    return d->longRunableCleanup.get();
}

ApplicationMetricsStorage* ApplicationContext::metrics() const
{
    return d->metricsStorage.get();
}

} // namespace nx::vms::common
