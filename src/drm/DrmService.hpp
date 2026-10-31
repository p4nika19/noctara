#pragma once

#include "drm/DrmBackend.hpp"
#include "drm/DrmHost.hpp"

#include <QObject>

namespace noctara::settings {
class SettingsStore;
}

namespace noctara::net {
class NetworkService;
}

namespace noctara::drm {

// Protected playback as the app uses it: the helper host and its playback backend, kept in step with the
// playback.protected.* settings and, live, with the network settings. GUI thread.
class DrmService : public QObject {
    Q_OBJECT

public:
    // `profileDir`: the helper's userData (<cache>/electron); `applicationDir`: where the helper is looked for.
    DrmService(settings::SettingsStore& settings, net::NetworkService& network, QString profileDir,
               QString applicationDir, QObject* parent = nullptr);

    [[nodiscard]] DrmHost& host() noexcept { return m_host; }
    [[nodiscard]] DrmBackend& backend() noexcept { return m_backend; }

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

private:
    void applySettings(const QString& key);
    void applyNetwork();

    settings::SettingsStore& m_settings;
    net::NetworkService& m_network;
    QString m_profileDir;
    QString m_applicationDir;
    DrmHost m_host;
    DrmBackend m_backend;
};

}  // namespace noctara::drm
