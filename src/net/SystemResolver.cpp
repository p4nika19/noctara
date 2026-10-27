#include "net/SystemResolver.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <memory>
#include <netdb.h>
#include <sys/socket.h>

namespace noctara::net {

namespace {

struct AddrInfoDeleter {
    void operator()(addrinfo* info) const noexcept { ::freeaddrinfo(info); }
};

Result<std::vector<IpAddress>> getAddresses(const QByteArray& host)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;
    addrinfo* raw = nullptr;
    const int rc = ::getaddrinfo(host.constData(), nullptr, &hints, &raw);
    const std::unique_ptr<addrinfo, AddrInfoDeleter> list(raw);
    if (rc == EAI_NONAME || rc == EAI_NODATA) {
        return fail(ErrorCode::NotFound, "system resolver: no such host");
    }
    if (rc != 0) {
        return fail(ErrorCode::DnsResolutionFailed, std::string("system resolver: ") + ::gai_strerror(rc));
    }
    std::vector<IpAddress> addresses;
    for (const addrinfo* it = list.get(); it != nullptr; it = it->ai_next) {
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr is a tagged union by design
        if (it->ai_family == AF_INET) {
            const auto* in = reinterpret_cast<const sockaddr_in*>(it->ai_addr);
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(&in->sin_addr);
            addresses.push_back(IpAddress::fromV4(std::span<const std::uint8_t, 4>(bytes, 4)));
        } else if (it->ai_family == AF_INET6) {
            const auto* in6 = reinterpret_cast<const sockaddr_in6*>(it->ai_addr);
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(&in6->sin6_addr);
            addresses.push_back(IpAddress::fromV6(std::span<const std::uint8_t, 16>(bytes, 16)));
        }
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    }
    std::vector<IpAddress> unique;
    for (const IpAddress& address : addresses) {
        if (std::ranges::find(unique, address) == unique.end()) {
            unique.push_back(address);
        }
    }
    if (unique.empty()) {
        return fail(ErrorCode::DnsResolutionFailed, "system resolver: no addresses");
    }
    return unique;
}

}  // namespace

SystemResolver::SystemResolver(QObject* parent)
    : QObject(parent)
{
    m_pool.setMaxThreadCount(4);
    m_pool.setExpiryTimeout(10'000);
}

SystemResolver::~SystemResolver()
{
    // getaddrinfo cannot be interrupted; pending lookups finish (bounded by the OS resolver timeout).
    m_pool.clear();
    m_pool.waitForDone();
}

void SystemResolver::lookup(const QString& host, std::function<void(Result<std::vector<IpAddress>>)> callback)
{
    QtConcurrent::run(&m_pool, getAddresses, host.toLatin1())
        .then(this,
              [callback = std::move(callback)](const Result<std::vector<IpAddress>>& result) { callback(result); });
}

}  // namespace noctara::net
