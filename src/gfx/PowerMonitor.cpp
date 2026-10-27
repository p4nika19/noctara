#include "gfx/PowerMonitor.hpp"

#include "core/Log.hpp"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>

namespace noctara::gfx {

namespace {

constexpr QLatin1StringView kService{"org.freedesktop.UPower"};
constexpr QLatin1StringView kDaemonPath{"/org/freedesktop/UPower"};
constexpr QLatin1StringView kDaemonInterface{"org.freedesktop.UPower"};
// The composite "display device" summarises all batteries; IsPresent is false on desktops.
constexpr QLatin1StringView kBatteryPath{"/org/freedesktop/UPower/devices/DisplayDevice"};
constexpr QLatin1StringView kDeviceInterface{"org.freedesktop.UPower.Device"};
constexpr QLatin1StringView kPropertiesInterface{"org.freedesktop.DBus.Properties"};

}  // namespace

bool energySavingActive(const QString& mode, PowerState state) noexcept
{
    if (mode == u"always") {
        return true;
    }
    if (mode == u"off") {
        return false;
    }
    return state == PowerState::OnBattery;
}

PowerMonitor::PowerMonitor(QObject* parent)
    : QObject(parent)
{
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        qCInfo(lcGfx) << "system D-Bus unavailable; power state unknown";
        return;
    }
    bus.connect(kService, kDaemonPath, kPropertiesInterface, QStringLiteral("PropertiesChanged"), this,
                SLOT(onDaemonPropertiesChanged(QString, QVariantMap, QStringList)));
    bus.connect(kService, kBatteryPath, kPropertiesInterface, QStringLiteral("PropertiesChanged"), this,
                SLOT(onBatteryPropertiesChanged(QString, QVariantMap, QStringList)));
    query(kDaemonPath, kDaemonInterface, QStringLiteral("OnBattery"));
    query(kBatteryPath, kDeviceInterface, QStringLiteral("IsPresent"));
}

PowerState PowerMonitor::state() const noexcept
{
    if (!m_reachable) {
        return PowerState::Unknown;
    }
    if (m_onBattery) {
        return PowerState::OnBattery;
    }
    return m_batteryPresent ? PowerState::OnMains : PowerState::NoBattery;
}

void PowerMonitor::query(const QString& path, const QString& interface, const QString& property)
{
    QDBusMessage call = QDBusMessage::createMethodCall(kService, path, kPropertiesInterface, QStringLiteral("Get"));
    call << interface << property;
    const auto* watcher =
        new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call),
                                    this);  // NOLINT(cppcoreguidelines-owning-memory): parented, deleteLater below
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, property](QDBusPendingCallWatcher* self) {
        const QDBusPendingReply<QDBusVariant> reply = *self;
        self->deleteLater();
        if (reply.isError()) {
            qCInfo(lcGfx).noquote() << "UPower query failed:" << reply.error().message();
            return;
        }
        const bool value = reply.value().variant().toBool();
        if (property == u"OnBattery") {
            update(true, value, std::nullopt);
        } else {
            update(true, std::nullopt, value);
        }
    });
}

void PowerMonitor::onDaemonPropertiesChanged(const QString& /*interface*/, const QVariantMap& changed,
                                             const QStringList& /*invalidated*/)
{
    if (const auto it = changed.constFind(QStringLiteral("OnBattery")); it != changed.constEnd()) {
        update(true, it->toBool(), std::nullopt);
    }
}

void PowerMonitor::onBatteryPropertiesChanged(const QString& /*interface*/, const QVariantMap& changed,
                                              const QStringList& /*invalidated*/)
{
    if (const auto it = changed.constFind(QStringLiteral("IsPresent")); it != changed.constEnd()) {
        update(true, std::nullopt, it->toBool());
    }
}

void PowerMonitor::update(bool reachable, std::optional<bool> onBattery, std::optional<bool> batteryPresent)
{
    const PowerState before = state();
    m_reachable = m_reachable || reachable;
    m_onBattery = onBattery.value_or(m_onBattery);
    m_batteryPresent = batteryPresent.value_or(m_batteryPresent);
    if (state() != before) {
        qCInfo(lcGfx) << "power state changed:" << static_cast<int>(state());
        Q_EMIT stateChanged();
    }
}

}  // namespace noctara::gfx
