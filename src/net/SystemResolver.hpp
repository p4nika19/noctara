#pragma once

#include "net/Resolver.hpp"

#include <QObject>
#include <QThreadPool>

namespace noctara::net {

// getaddrinfo() on a small private thread pool; results are delivered on the thread that owns this object.
// The OS gives no TTL, so the cache applies its minimum lifetime to these answers.
class SystemResolver : public QObject, public ISystemResolver {
    Q_OBJECT

public:
    explicit SystemResolver(QObject* parent = nullptr);
    ~SystemResolver() override;
    SystemResolver(const SystemResolver&) = delete;
    SystemResolver& operator=(const SystemResolver&) = delete;
    SystemResolver(SystemResolver&&) = delete;
    SystemResolver& operator=(SystemResolver&&) = delete;

    void lookup(const QString& host, std::function<void(Result<std::vector<IpAddress>>)> callback) override;

private:
    QThreadPool m_pool;
};

}  // namespace noctara::net
