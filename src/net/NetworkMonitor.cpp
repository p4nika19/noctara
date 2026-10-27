#include "net/NetworkMonitor.hpp"

#include "core/Log.hpp"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

namespace noctara::net {

namespace {

constexpr QLatin1StringView kService{"org.freedesktop.NetworkManager"};
constexpr QLatin1StringView kPath{"/org/freedesktop/NetworkManager"};
constexpr QLatin1StringView kInterface{"org.freedesktop.NetworkManager"};
constexpr QLatin1StringView kPropertiesInterface{"org.freedesktop.DBus.Properties"};

// NMConnectivityState and NMMetered values.
Connectivity connectivityFrom(uint value)
{
    switch (value) {
    case 1: return Connectivity::None;
    case 2: return Connectivity::Portal;
    case 3: return Connectivity::Limited;
    case 4: return Connectivity::Full;
    default: return Connectivity::Unknown;
    }
}

bool meteredFrom(uint value)
{
    return value == 1 || value == 3;  // NM_METERED_YES, NM_METERED_GUESS_YES
}

}  // namespace

NetworkMonitor::NetworkMonitor(QObject* parent)
    : QObject(parent)
{
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        qCInfo(lcNet) << "system D-Bus unavailable; connectivity and metered state unknown";
        return;
    }
    bus.connect(kService, kPath, kPropertiesInterface, QStringLiteral("PropertiesChanged"), this,
                SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
    QDBusMessage call = QDBusMessage::createMethodCall(kService, kPath, kPropertiesInterface, QStringLiteral("GetAll"));
    call << QString(kInterface);
    const auto* watcher =
        new QDBusPendingCallWatcher(bus.asyncCall(call),
                                    this);  // NOLINT(cppcoreguidelines-owning-memory): parented, deleteLater below
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* self) {
        const QDBusPendingReply<QVariantMap> reply = *self;
        self->deleteLater();
        if (reply.isError()) {
            qCInfo(lcNet).noquote() << "NetworkManager unavailable:" << reply.error().message();
            return;
        }
        apply(reply.value());
    });
}

void NetworkMonitor::onPropertiesChanged(const QString& /*interface*/, const QVariantMap& changed,
                                         const QStringList& /*invalidated*/)
{
    apply(changed);
}

void NetworkMonitor::apply(const QVariantMap& properties)
{
    const Connectivity connectivity = properties.contains(QStringLiteral("Connectivity"))
                                          ? connectivityFrom(properties.value(QStringLiteral("Connectivity")).toUInt())
                                          : m_connectivity;
    const bool metered = properties.contains(QStringLiteral("Metered"))
                             ? meteredFrom(properties.value(QStringLiteral("Metered")).toUInt())
                             : m_metered;
    if (connectivity != m_connectivity || metered != m_metered) {
        m_connectivity = connectivity;
        m_metered = metered;
        Q_EMIT changed();
    }
}

}  // namespace noctara::net
