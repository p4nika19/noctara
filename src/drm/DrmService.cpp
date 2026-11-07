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
#include <QTimer>

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
                       QString applicationDir, QString dataDir, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_network(network)
    , m_profileDir(std::move(profileDir))
    , m_applicationDir(std::move(applicationDir))
    , m_installer(network.transport(), std::move(dataDir), std::nullopt)
    , m_host(DrmHost::Options{
          .command = command(),
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
    // A protected track found no helper: set one up now (the track is reported as waiting for it).
    connect(&m_host, &DrmHost::helperMissing, this, [this] { installIfMissing(false); });
    connect(&m_installer, &HelperInstaller::progressChanged, this, &DrmService::statusChanged);
    connect(&m_installer, &HelperInstaller::finished, this, [this](bool ok) {
        m_host.setMissingReason(Error{ErrorCode::ProtectedPlayerMissing, "no helper installed"});
        if (ok) {
            m_host.setCommand(command());
            Q_EMIT notice(QStringLiteral("success"),
                          QCoreApplication::translate("Drm", "The protected player is set up: protected tracks play "
                                                             "now."));
        }
        Q_EMIT statusChanged();
    });
    // Without a helper, one is set up shortly after start (in the background: not on a metered connection).
    QTimer::singleShot(kInstallDelay, this, [this] { installIfMissing(true); });
}

std::optional<HelperCommand> DrmService::command() const
{
    if (auto found = findHelper(m_settings.stringValue(keys::ProtectedHelper), m_applicationDir)) {
        return found;
    }
    if (!m_settings.stringValue(keys::ProtectedHelper).isEmpty()) {
        return std::nullopt;  // a configured folder that is not usable is not replaced silently
    }
    return m_installer.installed();
}

void DrmService::installIfMissing(bool background)
{
    if (!m_settings.boolValue(keys::ProtectedEnabled) || !m_settings.boolValue(keys::ProtectedAutoInstall) ||
        !m_settings.stringValue(keys::ProtectedHelper).isEmpty() || command() || m_installer.running()) {
        return;
    }
    m_host.setMissingReason(Error{ErrorCode::ProtectedPlayerInstalling, "setting up the helper"});
    m_installer.install(background);
}

void DrmService::applySettings(const QString& key)
{
    if (key == QLatin1StringView(keys::ProtectedHelper)) {
        m_host.setCommand(command());
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
    if (m_host.processId() != 0) {
        // Running or already on its way out: Chromium writes parts of its profile as it exits, so the profile is
        // emptied once the helper is gone (a new start reaps the old process first, so this runs before it begins).
        connect(&m_host, &DrmHost::stopped, this, &DrmService::removeProfile, Qt::SingleShotConnection);
        m_host.shutdown();
        return;
    }
    removeProfile();
}

void DrmService::removeProfile()
{
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
    constexpr double kMegabyte = 1024.0 * 1024.0;
    if (m_installer.phase() == HelperInstaller::Phase::Downloading) {
        const auto total = static_cast<double>(m_installer.total());
        const auto received = static_cast<double>(m_installer.received());
        //: %1 = percent done, %2 = total size in MB
        lines << QCoreApplication::translate("Drm", "Setting up the protected player: downloading, %1 % of %2 MB…")
                     .arg(total > 0 ? qRound(received * 100.0 / total) : 0)
                     .arg(qRound(total / kMegabyte));
    } else if (m_installer.phase() == HelperInstaller::Phase::Unpacking) {
        lines << QCoreApplication::translate("Drm", "Setting up the protected player: unpacking…");
    } else if (!command()) {
        if (m_installer.phase() == HelperInstaller::Phase::Failed) {
            const ErrorCode code = m_installer.error().code;
            lines << (code == ErrorCode::MeteredDeferred
                          ? QCoreApplication::translate(
                                "Drm", "The protected player is set up once the connection is not metered.")
                          //: %1 = reason
                          : QCoreApplication::translate("Drm", "Could not set up the protected player: %1 It is "
                                                               "tried again when a protected track plays.")
                                .arg(i18n::errorMessage(code)));
        } else {
            lines << i18n::errorMessage(ErrorCode::ProtectedPlayerMissing);
        }
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
