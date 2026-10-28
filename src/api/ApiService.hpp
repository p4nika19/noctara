#pragma once

#include "api/ApiClient.hpp"
#include "api/ClientId.hpp"

#include <QObject>
#include <QThread>

namespace noctara::settings {
class SettingsStore;
}

namespace noctara::api {

class Session;

// Owns the API thread (client_id discovery, requests, JSON parsing) and keeps it in step with the soundcloud.*
// settings. Lives on the GUI thread; client() is thread-safe.
class ApiService : public QObject {
    Q_OBJECT

public:
    // `transport` should be the caching transport (cache::CacheService::transport()); `stateDir` keeps the last
    // discovered client_id (empty: memory only).
    ApiService(settings::SettingsStore& settings, net::INetworkTransport& transport, Session& session,
               const QString& stateDir, QObject* parent = nullptr);
    ~ApiService() override;
    ApiService(const ApiService&) = delete;
    ApiService& operator=(const ApiService&) = delete;
    ApiService(ApiService&&) = delete;
    ApiService& operator=(ApiService&&) = delete;

    [[nodiscard]] IApiClient& client() noexcept;
    // Items requested per page of a collection (library.page_size).
    [[nodiscard]] int pageSize() const;

private:
    void applyOverride();

    settings::SettingsStore& m_settings;
    QThread m_thread;
    QObject* m_anchor = nullptr;              // lives on m_thread
    ClientIdProvider* m_clientIds = nullptr;  // lives on m_thread; created and destroyed there
    ApiClient* m_client = nullptr;            // lives on m_thread; created and destroyed there
};

}  // namespace noctara::api
