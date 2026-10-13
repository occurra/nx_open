// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <cstdint>
#include <vector>

#include "../socket_common.h"

namespace nx::network {

enum class AddressAttributeType
{
    unknown,
    /** NX peer port. */
    port,
};

struct NX_NETWORK_API AddressAttribute
{
    AddressAttributeType type;
    std::uint64_t value; // TODO: boost::variant when int is not enough

    AddressAttribute(AddressAttributeType type_, std::uint64_t value_ = 0);
    bool operator==(const AddressAttribute& rhs) const;
    bool operator<(const AddressAttribute& rhs) const;
    std::string toString() const;
};

struct NX_NETWORK_API AddressEntry
{
    HostAddress host;
    std::vector<AddressAttribute> attributes;

    AddressEntry(HostAddress host_ = HostAddress());

    AddressEntry(const SocketAddress& address);

    bool operator==(const AddressEntry& rhs) const;
    bool operator<(const AddressEntry& rhs) const;
    std::string toString() const;
    SocketAddress toEndpoint() const;
};

inline std::ostream& operator<<(std::ostream& os, const AddressEntry& value)
{
    return os << value.toString();
}

} // namespace nx::network
