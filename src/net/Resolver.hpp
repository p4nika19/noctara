#pragma once

#include "core/Error.hpp"
#include "core/NetAddress.hpp"

#include <QString>

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

namespace noctara::net {

enum class ResolverSource : std::uint8_t { Literal, Override, Cache, System, SystemFallback, Servers, Doh };

[[nodiscard]] const char* resolverSourceName(ResolverSource source) noexcept;

struct Resolution {
    std::vector<IpAddress> addresses;  // ordered by the IP family preference
    ResolverSource source = ResolverSource::System;
    QString server;  // DNS server or DoH host that answered (empty for the system resolver)
    std::chrono::milliseconds elapsed{0};
};

using ResolveCallback = std::function<void(Result<Resolution>)>;

// Name resolution for all of Noctara's connections. Callbacks always run later on the resolver's thread,
// never inside resolve().
class IDnsResolver {
public:
    IDnsResolver() = default;
    virtual ~IDnsResolver() = default;
    IDnsResolver(const IDnsResolver&) = delete;
    IDnsResolver& operator=(const IDnsResolver&) = delete;
    IDnsResolver(IDnsResolver&&) = delete;
    IDnsResolver& operator=(IDnsResolver&&) = delete;

    virtual void resolve(const QString& host, ResolveCallback callback) = 0;
};

// The operating system's resolver (getaddrinfo), separate so tests can prove when it is (not) called.
// Reports ErrorCode::NotFound when the name does not exist and DnsResolutionFailed for other failures.
class ISystemResolver {
public:
    ISystemResolver() = default;
    virtual ~ISystemResolver() = default;
    ISystemResolver(const ISystemResolver&) = delete;
    ISystemResolver& operator=(const ISystemResolver&) = delete;
    ISystemResolver(ISystemResolver&&) = delete;
    ISystemResolver& operator=(ISystemResolver&&) = delete;

    virtual void lookup(const QString& host, std::function<void(Result<std::vector<IpAddress>>)> callback) = 0;
};

}  // namespace noctara::net
