#pragma once

#include <QDBusConnection>
#include <QDBusContext>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace noctara::media {

// Watches the session bus for other MPRIS players that start playing: a video in the browser, another music
// player. Noctara pauses then when the "pause for other players" setting is on. Only real players count: our own
// bus connection and proxies that mirror whichever player is active (playerctld, which would mirror Noctara
// itself) are ignored. A player counts when its status turns to "Playing", and when it appears already playing.
class OtherPlayers : public QObject, protected QDBusContext {
    Q_OBJECT

public:
    explicit OtherPlayers(QDBusConnection bus, QObject* parent = nullptr);

    void setEnabled(bool enabled);
    [[nodiscard]] bool enabled() const noexcept { return m_enabled; }

    // The bus events, also driven directly by tests. `name` is a well-known MPRIS name, `owner` and `sender` are
    // unique connection names.
    void noteOwner(const QString& name, const QString& owner);
    void reportStatus(const QString& sender, const QString& status);

    // Mirror whichever player is active rather than playing anything themselves.
    [[nodiscard]] static bool isProxy(const QString& name);

Q_SIGNALS:
    // `name`: the player's well-known bus name (org.mpris.MediaPlayer2.<player>).
    void startedPlaying(const QString& name);

private Q_SLOTS:
    void onPropertiesChanged(const QString& interface, const QVariantMap& changed, const QStringList& invalidated);
    void onNameOwnerChanged(const QString& name, const QString& oldOwner, const QString& newOwner);

private:
    void askStatus(const QString& name);

    QDBusConnection m_bus;
    bool m_enabled = false;
    QHash<QString, QString> m_owners;    // well-known MPRIS name -> unique owner
    QHash<QString, QString> m_statuses;  // unique owner -> last PlaybackStatus
};

}  // namespace noctara::media
