#include "api/ApiService.hpp"

#include "core/Handoff.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"

namespace noctara::api {

ApiService::ApiService(settings::SettingsStore& settings, net::INetworkTransport& transport, Session& session,
                       const QString& stateDir, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_anchor(new QObject)  // NOLINT(cppcoreguidelines-owning-memory): deleted on the API thread
{
    m_thread.setObjectName(QStringLiteral("noctara-api"));
    m_thread.start();
    m_anchor->moveToThread(&m_thread);
    const QString storeFile = stateDir.isEmpty() ? QString() : stateDir + QStringLiteral("/soundcloud.json");
    handoff::runAndWait(m_anchor, [&] {
        // NOLINTBEGIN(cppcoreguidelines-owning-memory): deleted on the API thread in the destructor
        m_clientIds = new ClientIdProvider(transport, storeFile);
        m_client = new ApiClient(transport, session, *m_clientIds);
        // NOLINTEND(cppcoreguidelines-owning-memory)
    });
    applyOverride();
    connect(&m_settings, &settings::SettingsStore::valueChanged, this, [this](const QString& key) {
        if (key == QLatin1StringView(settings::keys::ClientId)) {
            applyOverride();
        }
    });
}

ApiService::~ApiService()
{
    handoff::runAndWait(m_anchor, [this] {
        // The provider first: its pending waiters refer to the client and are dropped with it.
        delete std::exchange(m_clientIds, nullptr);  // NOLINT(cppcoreguidelines-owning-memory)
        delete std::exchange(m_client, nullptr);     // NOLINT(cppcoreguidelines-owning-memory)
        delete std::exchange(m_anchor, nullptr);     // NOLINT(cppcoreguidelines-owning-memory)
    });
    m_thread.quit();
    m_thread.wait();
}

IApiClient& ApiService::client() noexcept
{
    return *m_client;  // send() and lookupTracks() are thread-safe
}

int ApiService::pageSize() const
{
    return static_cast<int>(m_settings.intValue(settings::keys::PageSize));
}

void ApiService::applyOverride()
{
    handoff::post(m_clientIds, [clientIds = m_clientIds, id = m_settings.stringValue(settings::keys::ClientId)] {
        clientIds->setOverride(id.trimmed());
    });
}

}  // namespace noctara::api
