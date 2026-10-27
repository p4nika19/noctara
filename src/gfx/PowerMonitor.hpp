#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

namespace noctara::gfx {

enum class PowerState : std::uint8_t {
    Unknown,    // UPower unavailable
    NoBattery,  // desktop / no battery present
    OnMains,
    OnBattery,
};

// "auto" | "always" | "off" (performance.power.energy_saving) + power state -> whether to save energy.
[[nodiscard]] bool energySavingActive(const QString& mode, PowerState state) noexcept;

// Tracks whether the machine runs on battery via UPower on the system D-Bus. Event-driven: initial
// values are fetched asynchronously, updates arrive through PropertiesChanged (no polling).
class PowerMonitor : public QObject {
    Q_OBJECT

public:
    explicit PowerMonitor(QObject* parent = nullptr);

    [[nodiscard]] PowerState state() const noexcept;

Q_SIGNALS:
    void stateChanged();

private Q_SLOTS:
    void onDaemonPropertiesChanged(const QString& interface, const QVariantMap& changed,
                                   const QStringList& invalidated);
    void onBatteryPropertiesChanged(const QString& interface, const QVariantMap& changed,
                                    const QStringList& invalidated);

private:
    void query(const QString& path, const QString& interface, const QString& property);
    void update(bool reachable, std::optional<bool> onBattery, std::optional<bool> batteryPresent);

    bool m_reachable = false;
    bool m_onBattery = false;
    bool m_batteryPresent = false;
};

}  // namespace noctara::gfx
