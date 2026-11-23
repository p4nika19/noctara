#pragma once

#include "drm/DrmBackend.hpp"
#include "drm/DrmHost.hpp"
#include "drm/HelperInstaller.hpp"
#include "drm/SocksRelay.hpp"

#include <QObject>

namespace noctara::settings {
class SettingsStore;
}

namespace noctara::net {
class NetworkService;
}

namespace noctara::drm {

// Protected playback as the app uses it: the helper host and its playback backend, kept in step with the
// playback.protected.* settings and, live, with the network settings, and the installer that sets the helper up
// when none is found (shortly after start in the background, or at once when a protected track needs it). GUI
// thread.
class DrmService : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool installed READ isInstalled NOTIFY statusChanged)
    Q_PROPERTY(bool installing READ isInstalling NOTIFY statusChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)

public:
    // `profileDir`: the helper's userData (<cache>/electron); `applicationDir`: where a helper shipped with
    // Noctara is looked for; `dataDir`: where Noctara installs one itself.
    DrmService(settings::SettingsStore& settings, net::NetworkService& network, QString profileDir,
               QString applicationDir, QString dataDir, QObject* parent = nullptr);

    [[nodiscard]] DrmHost& host() noexcept { return m_host; }
    [[nodiscard]] DrmBackend& backend() noexcept { return m_backend; }
    [[nodiscard]] HelperInstaller& installer() noexcept { return m_installer; }

    [[nodiscard]] bool isInstalled() const { return command().has_value(); }
    [[nodiscard]] bool isInstalling() const { return m_installer.running(); }

    Q_INVOKABLE void install(bool force = false, bool direct = false);
    Q_INVOKABLE void reinstall(bool direct = false);

    // How long after start a missing helper is set up in the background.
    static constexpr std::chrono::seconds kInstallDelay{30};

    // One translated line for Settings → Playback → Protected tracks: whether the helper is installed, running or
    // failed, and which network settings it cannot follow.
    [[nodiscard]] QString statusText() const;
    // Which network settings protected tracks do not follow, as one translated sentence; empty when none.
    [[nodiscard]] QString networkWarning() const;

    // Uses the profile on the next start. An idle helper is stopped now; active playback is not interrupted.
    void setProfileDir(QString profileDir);
    // "Clear all caches": stops an idle helper and empties its profile, except the Widevine component itself.
    // A helper playing a track keeps its profile until the next clear.
    void clearProfile();

Q_SIGNALS:
    void statusChanged();
    void directDownloadPromptRequested();
    // Something to tell the user (kind: "success" | "warning" | "error"), e.g. that the player was set up.
    void notice(const QString& kind, const QString& message);

private:
    void applySettings(const QString& key);
    void applyNetwork();
    [[nodiscard]] HelperNetwork networkPlan();
    void removeProfile();
    [[nodiscard]] std::optional<HelperCommand> command() const;
    void installIfMissing(bool background);

    settings::SettingsStore& m_settings;
    net::NetworkService& m_network;
    QString m_profileDir;
    QString m_applicationDir;
    HelperInstaller m_installer;
    SocksRelay m_relay;  // outlives the host: the helper's connections go through it
    DrmHost m_host;
    DrmBackend m_backend;
};

}  // namespace noctara::drm
