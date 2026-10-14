// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "socket_factory.h"

#include <iostream>

#include <nx/network/nx_network_ini.h>
#include <nx/network/socket_global.h>
#include <nx/network/ssl/context.h>
#include <nx/network/ssl/ssl_stream_socket.h>
#include <nx/network/system_socket.h>
#include <nx/utils/basic_factory.h>

namespace nx::network {

namespace {

static SocketFactory::CreateStreamSocketFuncType createStreamSocketFunc = [](auto&&... args)
{
    return SocketFactory::defaultStreamSocketFactoryFunc(std::forward<decltype(args)>(args)...);
};

static SocketFactory::CreateStreamServerSocketFuncType createStreamServerSocketFunc =
    [](auto&&... args)
{
    return SocketFactory::defaultStreamServerSocketFactoryFunc(
        std::forward<decltype(args)>(args)...);
};

} // namespace

namespace ssl {

const AdapterFunc kDefaultCertificateCheck = [](auto socket)
{
    return std::make_unique<ClientStreamSocket>(
        Context::instance(),
        std::move(socket),
        kDefaultCertificateCheckCallback);
};

const AdapterFunc kAcceptAnyCertificate = [](auto socket)
{
    return std::make_unique<ClientStreamSocket>(
        Context::instance(),
        std::move(socket),
        kAcceptAnyCertificateCallback);
};

const AdapterFunc kRejectAnyCertificate = [](auto socket)
{
    return std::make_unique<ClientStreamSocket>(
        Context::instance(),
        std::move(socket),
        [](auto...)
        {
            return false;
        });
};

} // namespace ssl

class SocketFactoryImpl
{
public:
    using DatagramSocketFactory = nx::utils::
        BasicAbstractObjectFactory<AbstractDatagramSocket, UDPSocket, int /*e.g., AF_INET*/>;

    DatagramSocketFactory datagramSocketFactory;
};

//-------------------------------------------------------------------------------------------------
// Datagram factory.

std::unique_ptr<AbstractDatagramSocket> SocketFactory::createDatagramSocket()
{
    return instance().m_impl->datagramSocketFactory.create(s_udpIpVersion.load());
}

SocketFactory::DatagramSocketFactoryFunc SocketFactory::setCustomDatagramSocketFactoryFunc(
    DatagramSocketFactoryFunc func)
{
    return instance().m_impl->datagramSocketFactory.setCustomFunc(std::move(func));
}

//-------------------------------------------------------------------------------------------------

std::unique_ptr<AbstractStreamSocket> SocketFactory::createStreamSocket(
    ssl::AdapterFunc adapterFunc,
    bool sslRequired,
    std::optional<int> ipVersion)
{
    return createStreamSocketFunc(std::move(adapterFunc), sslRequired, ipVersion);
}

std::unique_ptr<AbstractStreamServerSocket> SocketFactory::createStreamServerSocket(
    bool sslRequired,
    std::optional<int> ipVersion)
{
    return createStreamServerSocketFunc(sslRequired, ipVersion);
}

std::unique_ptr<nx::network::AbstractStreamServerSocket> SocketFactory::createSslAdapter(
    std::unique_ptr<nx::network::AbstractStreamServerSocket> serverSocket,
    ssl::Context* context,
    ssl::EncryptionUse encryptionUse)
{
    return std::make_unique<ssl::StreamServerSocket>(
        context,
        std::move(serverSocket),
        encryptionUse);
}

SocketFactory::CreateStreamSocketFuncType SocketFactory::setCreateStreamSocketFunc(
    CreateStreamSocketFuncType newFactoryFunc)
{
    return std::exchange(createStreamSocketFunc, std::move(newFactoryFunc));
}

SocketFactory::CreateStreamServerSocketFuncType SocketFactory::setCreateStreamServerSocketFunc(
    CreateStreamServerSocketFuncType newFactoryFunc)
{
    return std::exchange(createStreamServerSocketFunc, std::move(newFactoryFunc));
}

void SocketFactory::setIpVersion(const std::string& ipVersion)
{
    if (ipVersion.empty())
        return;

    NX_INFO(typeid(SocketFactory), "%1(%2)", __func__, ipVersion);

    if (ipVersion == "4")
    {
        s_udpIpVersion = AF_INET;
        s_tcpClientIpVersion = AF_INET;
        s_tcpServerIpVersion = AF_INET;
        return;
    }

    if (ipVersion == "6")
    {
        s_udpIpVersion = AF_INET6;
        s_tcpClientIpVersion = AF_INET6;
        s_tcpServerIpVersion = AF_INET6;
        return;
    }

    if (ipVersion == "6tcp") /** TCP only */
    {
        s_udpIpVersion = AF_INET;
        s_tcpClientIpVersion = AF_INET6;
        s_tcpServerIpVersion = AF_INET6;
        return;
    }

    if (ipVersion == "6server") /** Server only */
    {
        s_udpIpVersion = AF_INET;
        s_tcpClientIpVersion = AF_INET;
        s_tcpServerIpVersion = AF_INET6;
        return;
    }

    std::cerr << "Unsupported IP version: " << ipVersion << std::endl;
    ::abort();
}

nx::network::IpVersion SocketFactory::setUdpIpVersion(nx::network::IpVersion ipVersion)
{
    const int prevVersion = s_udpIpVersion.load();
    s_udpIpVersion = static_cast<int>(ipVersion);
    return static_cast<nx::network::IpVersion>(prevVersion);
}

int SocketFactory::udpIpVersion()
{
    return s_udpIpVersion;
}

int SocketFactory::tcpClientIpVersion()
{
    return s_tcpClientIpVersion;
}

int SocketFactory::tcpServerIpVersion()
{
    return s_tcpServerIpVersion;
}

SocketFactory& SocketFactory::instance()
{
    static SocketFactory staticInstance;
    return staticInstance;
}

SocketFactory::SocketFactory():
    m_impl(std::make_unique<SocketFactoryImpl>())
{
}

SocketFactory::~SocketFactory() = default;

#if TARGET_OS_IPHONE
std::atomic<int> SocketFactory::s_tcpServerIpVersion(AF_INET6);
std::atomic<int> SocketFactory::s_tcpClientIpVersion(AF_INET6);
std::atomic<int> SocketFactory::s_udpIpVersion(AF_INET6);
#else
std::atomic<int> SocketFactory::s_tcpServerIpVersion(AF_INET);
std::atomic<int> SocketFactory::s_tcpClientIpVersion(AF_INET);
std::atomic<int> SocketFactory::s_udpIpVersion(AF_INET);
#endif

std::unique_ptr<AbstractStreamSocket> SocketFactory::defaultStreamSocketFactoryFunc(
    ssl::AdapterFunc adapterFunc,
    bool sslRequired,
    std::optional<int> _ipVersion)
{
    auto ipVersion = (bool) _ipVersion ? *_ipVersion : s_tcpClientIpVersion.load();
    std::unique_ptr<AbstractStreamSocket> result = std::make_unique<TCPSocket>(ipVersion);

#ifdef ENABLE_SSL
    if (result && sslRequired)
        return adapterFunc(std::move(result));
#endif // ENABLE_SSL

    return result;
}

std::unique_ptr<AbstractStreamServerSocket> SocketFactory::defaultStreamServerSocketFactoryFunc(
    bool sslRequired,
    std::optional<int> _ipVersion)
{
    auto ipVersion = (bool) _ipVersion ? *_ipVersion : s_tcpServerIpVersion.load();
    std::unique_ptr<AbstractStreamServerSocket> result =
        std::make_unique<TCPServerSocket>(ipVersion);

#ifdef ENABLE_SSL
    if (result && sslRequired)
        return createSslAdapter(
            std::move(result),
            ssl::Context::instance(),
            ssl::EncryptionUse::autoDetectByReceivedData);
#endif // ENABLE_SSL

    return result;
}

} // namespace nx::network
