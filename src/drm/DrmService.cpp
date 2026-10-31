#include "drm/DrmService.hpp"

#include "core/Log.hpp"
#include "i18n/ErrorMessages.hpp"
#include "net/NetworkService.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace noctara::drm {

namespace keys = settings::keys;

namespace {

QString limitText(NetworkLimit limit)
{
    switch (limit) {
    case NetworkLimit::CustomDns:
        return QCoreApplication::translate("Drm", "custom DNS servers (it uses the system resolver)");
    case NetworkLimit::ProxyResolvesHere:
        return QCoreApplication::translate("Drm", "SOCKS4a (it resolves names itself and connects over SOCKS4)");
    case NetworkLimit::SocksCredentials: return QCoreApplication::translate("Drm", "SOCKS proxy sign-in");
    case NetworkLimit::CaBundle:
        return QCoreApplication::translate("Drm", "the CA bundle (it trusts the system's certificates)");
    case NetworkLimit::BandwidthLimit: return QCoreApplication::translate("Drm", "the bandwidth limit");
    case NetworkLimit::IpFamily: return QCoreApplication::translate("Drm", "IPv4-only or IPv6-only");
    }
    return {};
}

}  // namespace

DrmService::DrmService(settings::SettingsStore& settings, net::NetworkService& network, QString profileDir,
                       QString applicationDir, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_network(network)
    , m_profileDir(std::move(profileDir))
    , m_applicationDir(std::move(applicationDir))
    , m_host(DrmHost::Options{
          .command = findHelper(settings.stringValue(keys::ProtectedHelper), m_applicationDir),
          .profileDir = m_profileDir,
          .gpu = settings.boolValue(keys::ProtectedGpu),
          .idleTimeout = std::chrono::minutes(settings.intValue(keys::ProtectedIdle)),
      })
    , m_backend(m_host)
{
    m_host.setNetwork(planNetwork(m_network.config()));
    connect(&m_network, &net::NetworkService::configChanged, this, &DrmService::applyNetwork);
    connect(&m_settings, &settings::SettingsStore::valueChanged, this, &DrmService::applySettings);
    connect(&m_host, &DrmHost::statusChanged, this, &DrmService::statusChanged);
}

void DrmService::applySettings(const QString& key)
{
    if (key == QLatin1StringView(keys::ProtectedHelper)) {
        m_host.setCommand(findHelper(m_settings.stringValue(keys::ProtectedHelper), m_applicationDir));
        if (m_backend.state() == media::BackendState::Idle) {
            m_host.shutdown();  // the next protected track starts the new one
        }
        Q_EMIT statusChanged();
    } else if (key == QLatin1StringView(keys::ProtectedIdle)) {
        m_host.setIdleTimeout(std::chrono::minutes(m_settings.intValue(keys::ProtectedIdle)));
    } else if (key == QLatin1StringView(keys::ProtectedGpu)) {
        m_host.setGpu(m_settings.boolValue(keys::ProtectedGpu));
    } else if (key == QLatin1StringView(keys::ProtectedEnabled)) {
        if (!m_settings.boolValue(keys::ProtectedEnabled) && m_backend.state() == media::BackendState::Idle) {
            m_host.shutdown();
        }
        Q_EMIT statusChanged();
    }
}

QString DrmService::networkWarning() const
{
    const std::vector<NetworkLimit>& limits = m_host.network().limits;
    if (limits.empty()) {
        return {};
    }
    QStringList names;
    for (const NetworkLimit limit : limits) {
        names << limitText(limit);
    }
    //: %1 = list of network settings, e.g. "custom DNS servers (…); the bandwidth limit"
    return QCoreApplication::translate("Drm", "Protected tracks do not follow: %1.").arg(names.join(u"; "));
}

void DrmService::clearProfile()
{
    if (m_backend.state() != media::BackendState::Idle || m_profileDir.isEmpty()) {
        return;
    }
    m_host.shutdown();
    // The helper is on its way out; Chromium only writes its profile while running, and a new start waits for the
    // old process to be gone.
    const QDir profile(m_profileDir);
    for (const QFileInfo& entry : profile.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)) {
        if (entry.fileName() == u"WidevineCdm") {
            continue;  // the CDM component, not user data; it would only be downloaded again
        }
        const bool removed = entry.isDir() && !entry.isSymLink() ? QDir(entry.filePath()).removeRecursively()
                                                                 : QFile::remove(entry.filePath());
        if (!removed) {
            qCWarning(lcDrm).noquote() << "could not remove" << entry.filePath();
        }
    }
}

void DrmService::applyNetwork()
{
    m_host.setNetwork(planNetwork(m_network.config()));
    Q_EMIT statusChanged();
}

QString DrmService::statusText() const
{
    if (!m_settings.boolValue(keys::ProtectedEnabled)) {
        return {};
    }
    QStringList lines;
    const HelperNetwork& network = m_host.network();
    if (!findHelper(m_settings.stringValue(keys::ProtectedHelper), m_applicationDir)) {
        lines << i18n::errorMessage(ErrorCode::ProtectedPlayerMissing);
    } else if (network.blocked) {
        lines << i18n::errorMessage(network.blocked->code);
    } else {
        switch (m_host.status()) {
        case DrmHost::Status::Ready:
            //: %1 = Electron version
            lines << QCoreApplication::translate("Drm", "Running: castlabs Electron %1 with Widevine.")
                         .arg(m_host.electronVersion());
            break;
        case DrmHost::Status::Starting: lines << QCoreApplication::translate("Drm", "Starting…"); break;
        case DrmHost::Status::Failed: lines << i18n::errorMessage(m_host.lastError().code); break;
        case DrmHost::Status::Stopped:
            lines << QCoreApplication::translate("Drm", "Installed. It starts when a protected track plays.");
            break;
        }
    }
    if (const QString warning = networkWarning(); !warning.isEmpty()) {
        lines << warning;
    }
    return lines.join(u' ');
}

}  // namespace noctara::drm
