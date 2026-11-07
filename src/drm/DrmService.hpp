#pragma once

#include "drm/DrmBackend.hpp"
#include "drm/DrmHost.hpp"
#include "drm/HelperInstaller.hpp"

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

public:
    // `profileDir`: the helper's userData (<cache>/electron); `applicationDir`: where a helper shipped with
    // Noctara is looked for; `dataDir`: where Noctara installs one itself.
    DrmService(settings::SettingsStore& settings, net::NetworkService& network, QString profileDir,
               QString applicationDir, QString dataDir, QObject* parent = nullptr);

    [[nodiscard]] DrmHost& host() noexcept { return m_host; }
    [[nodiscard]] DrmBackend& backend() noexcept { return m_backend; }
    [[nodiscard]] HelperInstaller& installer() noexcept { return m_installer; }

    // How long after start a missing helper is set up in the background.
    static constexpr std::chrono::seconds kInstallDelay{30};

    // One translated line for Settings → Playback → Protected tracks: whether the helper is installed, running or
    // failed, and which network settings it cannot follow.
    [[nodiscard]] QString statusText() const;
    // Which network settings protected tracks do not follow, as one translated sentence; empty when none.
    [[nodiscard]] QString networkWarning() const;

    // "Clear all caches": stops an idle helper and empties its profile, except the Widevine component itself.
    // A helper playing a track keeps its profile until the next clear.
    void clearProfile();

Q_SIGNALS:
    void statusChanged();
    // Something to tell the user (kind: "success" | "warning" | "error"), e.g. that the player was set up.
    void notice(const QString& kind, const QString& message);

private:
    void applySettings(const QString& key);
    void applyNetwork();
    void removeProfile();
    [[nodiscard]] std::optional<HelperCommand> command() const;
    void installIfMissing(bool background);

    settings::SettingsStore& m_settings;
    net::NetworkService& m_network;
    QString m_profileDir;
    QString m_applicationDir;
    HelperInstaller m_installer;
    DrmHost m_host;
    DrmBackend m_backend;
};

}  // namespace noctara::drm
