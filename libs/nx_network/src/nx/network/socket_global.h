// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <nx/kit/ini_config.h>
#include <nx/utils/argument_parser.h>
#include <nx/utils/debug/allocation_analyzer.h>
#include <nx/utils/log/log.h>

#include "debug/object_counters.h"
#include "socket_common.h"

namespace nx::network {

namespace aio { class AIOService; }
class AddressResolver;

namespace http { class GlobalContext; }

struct SocketGlobalsImpl;

//-------------------------------------------------------------------------------------------------

/**
 * Contains objects that are required to use the network subsystem.
 * This object MUST be created before the first socket usage and destroyed after the last socket
 * usage. It can be created/destroyed with <pre><code> nx::network::SocketGlobalsHolder
 * socketGlobals;
 * ...
 * </code></pre>
 * or
 * <pre><code>
 * nx::network::SocketGlobals::init();
 * ...
 * nx::network::SocketGlobals::deinit();
 * </code></pre>
 */
class NX_NETWORK_API SocketGlobals
{
public:
    static aio::AIOService& aioService();
    static AddressResolver& addressResolver();
    static http::GlobalContext& httpGlobalContext();

    /**
     * Allocates required resources. E.g., starts AIO threads.
     * Must be called before any socket use.
     * @param arguments Command-line arguments
     * The following command-line arguments are supported:
     * - aio-thread-pool-size Number of AIO threads to create. By default, 0 (the number of threads
     * is auto-detected)
     */
    static void init(const ArgumentParser& arguments);

    /**
     * Must be called before any socket use.
     */
    static void init();

    static void deinit(); /**< Should be called when sockets are not needed any more. */
    static void verifyInitialization();
    static bool isInitialized();

    static void printArgumentsHelp(std::ostream* outputStream);
    static void applyArguments(const ArgumentParser& arguments);

    /**
     * Block sockets from connecting to a host that matches the regular expression given with
     * hostnameRegexp argument.
     */
    void blockHost(const std::string& hostnameRegexp);

    /**
     * Remove hostnameRegexp from the list of blocked hosts.
     */
    void unblockHost(const std::string& hostnameRegexp);

    /**
     * @return true if address has been blocked with SocketGlobals::blockHost.
     */
    bool isHostBlocked(const HostAddress& address) const;

    const debug::ObjectCounters& debugCounters() const;
    debug::ObjectCounters& debugCounters();

    nx::utils::debug::AllocationAnalyzer& allocationAnalyzer();

    static SocketGlobals& instance();

    class InitGuard
    {
    public:
        InitGuard() { init(); }
        InitGuard(const ArgumentParser& arguments) { init(arguments); }
        ~InitGuard() { deinit(); }

        InitGuard(const InitGuard&) = delete;
        InitGuard(InitGuard&&) = delete;
        InitGuard& operator=(const InitGuard&) = delete;
        InitGuard& operator=(InitGuard&&) = delete;
    };

private:
    std::unique_ptr<SocketGlobalsImpl> m_implGuard;
    SocketGlobalsImpl* m_impl = nullptr;
    debug::ObjectCounters m_debugCounters;
    nx::utils::debug::AllocationAnalyzer m_allocationAnalyzer;

    SocketGlobals();
    ~SocketGlobals();

    SocketGlobals(const SocketGlobals&) = delete;
    SocketGlobals& operator=(const SocketGlobals&) = delete;

    void reloadIni();

    void initializeNetworking(const ArgumentParser& arguments);
};

//-------------------------------------------------------------------------------------------------

/**
 * Invokes SocketGlobals::init() when constructed and SocketGlobals::deinit() just before
 * destruction.
 */
class NX_NETWORK_API SocketGlobalsHolder
{
public:
    /**
     * Invokes SocketGlobalsHolder::initialize().
     */
    SocketGlobalsHolder();
    SocketGlobalsHolder(const ArgumentParser& arguments);
    ~SocketGlobalsHolder();

    void initialize();
    void uninitialize();
    void reinitialize();

    static SocketGlobalsHolder* instance();

private:
    const nx::ArgumentParser m_args;
    std::unique_ptr<SocketGlobals::InitGuard> m_socketGlobalsGuard;
};

} // namespace nx::network
