#include "media/OtherPlayers.hpp"

#include "core/Log.hpp"

#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusVariant>

#include <utility>

namespace noctara::media {

namespace {

constexpr QLatin1StringView kMprisPrefix("org.mpris.MediaPlayer2.");
constexpr QLatin1StringView kObjectPath("/org/mpris/MediaPlayer2");
constexpr QLatin1StringView kPlayerInterface("org.mpris.MediaPlayer2.Player");
constexpr QLatin1StringView kPropertiesInterface("org.freedesktop.DBus.Properties");
constexpr QLatin1StringView kBusService("org.freedesktop.DBus");
constexpr QLatin1StringView kBusPath("/org/freedesktop/DBus");
constexpr QLatin1StringView kPlaying("Playing");

}  // namespace

OtherPlayers::OtherPlayers(QDBusConnection bus, QObject* parent)
    : QObject(parent)
    , m_bus(std::move(bus))
{
}

bool OtherPlayers::isProxy(const QString& name)
{
    return name == u"org.mpris.MediaPlayer2.playerctld";
}

void OtherPlayers::setEnabled(bool enabled)
{
    if (enabled == m_enabled) {
        return;
    }
    m_enabled = enabled;
    m_owners.clear();
    m_statuses.clear();
    if (!m_bus.isConnected()) {
        return;
    }
    const auto propertiesChanged = QStringLiteral("PropertiesChanged");
    const auto nameOwnerChanged = QStringLiteral("NameOwnerChanged");
    if (!enabled) {
        m_bus.disconnect(QString(), kObjectPath, kPropertiesInterface, propertiesChanged, this,
                         SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
        m_bus.disconnect(kBusService, kBusPath, kBusService, nameOwnerChanged, this,
                         SLOT(onNameOwnerChanged(QString, QString, QString)));
        return;
    }
    // Any sender: players are told apart by the names their connection owns.
    m_bus.connect(QString(), kObjectPath, kPropertiesInterface, propertiesChanged, this,
                  SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
    m_bus.connect(kBusService, kBusPath, kBusService, nameOwnerChanged, this,
                  SLOT(onNameOwnerChanged(QString, QString, QString)));
    // The players already there. Their current status is not asked for: one playing before the setting was turned
    // on is no reason to pause now.
    if (const QDBusConnectionInterface* bus = m_bus.interface()) {
        const QDBusReply<QStringList> names = bus->registeredServiceNames();
        for (const QString& name : names.value()) {
            if (name.startsWith(kMprisPrefix)) {
                noteOwner(name, bus->serviceOwner(name).value());
            }
        }
    }
}

void OtherPlayers::noteOwner(const QString& name, const QString& owner)
{
    if (owner.isEmpty()) {
        m_owners.remove(name);
    } else {
        m_owners.insert(name, owner);
    }
}

void OtherPlayers::reportStatus(const QString& sender, const QString& status)
{
    if (!m_enabled || (m_bus.isConnected() && sender == m_bus.baseService())) {
        return;
    }
    // The player's own name; a connection without one (or with a proxy's) is not a player of its own. Its status
    // is not remembered either: the name may only be announced after this (askStatus() then reports it).
    QString player;
    for (auto it = m_owners.cbegin(); it != m_owners.cend(); ++it) {
        if (it.value() != sender) {
            continue;
        }
        if (isProxy(it.key())) {
            return;
        }
        player = it.key();
    }
    if (player.isEmpty()) {
        return;
    }
    const QString previous = std::exchange(m_statuses[sender], status);
    if (status != kPlaying || previous == kPlaying) {
        return;
    }
    qCInfo(lcMedia).noquote() << "another player started:" << player;
    Q_EMIT startedPlaying(player);
}

void OtherPlayers::onPropertiesChanged(const QString& interface, const QVariantMap& changed,
                                       const QStringList& /*invalidated*/)
{
    if (interface != kPlayerInterface || !changed.contains(QStringLiteral("PlaybackStatus"))) {
        return;
    }
    reportStatus(message().service(), changed.value(QStringLiteral("PlaybackStatus")).toString());
}

void OtherPlayers::onNameOwnerChanged(const QString& name, const QString& oldOwner, const QString& newOwner)
{
    if (!name.startsWith(kMprisPrefix)) {
        return;
    }
    if (!oldOwner.isEmpty()) {
        m_statuses.remove(oldOwner);
    }
    noteOwner(name, newOwner);
    if (!newOwner.isEmpty()) {
        askStatus(name);  // a player may appear already playing
    }
}

void OtherPlayers::askStatus(const QString& name)
{
    QDBusMessage get = QDBusMessage::createMethodCall(name, kObjectPath, kPropertiesInterface, QStringLiteral("Get"));
    get << kPlayerInterface << QStringLiteral("PlaybackStatus");
    const auto* watcher =
        new QDBusPendingCallWatcher(m_bus.asyncCall(get),
                                    this);  // NOLINT(cppcoreguidelines-owning-memory): parented, deleteLater below
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, name](QDBusPendingCallWatcher* call) {
        call->deleteLater();
        const QDBusPendingReply<QDBusVariant> reply = *call;
        const QString owner = m_owners.value(name);
        if (reply.isValid() && !owner.isEmpty()) {
            reportStatus(owner, reply.value().variant().toString());
        }
    });
}

}  // namespace noctara::media
