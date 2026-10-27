#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantMap>

#include <cstdint>

namespace noctara::net {

enum class Connectivity : std::uint8_t { Unknown, None, Portal, Limited, Full };

// NetworkManager's view of the current connection on the system D-Bus: connectivity and whether the
// connection is metered (including NetworkManager's guesses, e.g. for phone hotspots). Event-driven; both
// values stay Unknown / false when NetworkManager is not running.
class NetworkMonitor : public QObject {
    Q_OBJECT

public:
    explicit NetworkMonitor(QObject* parent = nullptr);

    [[nodiscard]] Connectivity connectivity() const noexcept { return m_connectivity; }
    [[nodiscard]] bool metered() const noexcept { return m_metered; }

Q_SIGNALS:
    void changed();

private Q_SLOTS:
    void onPropertiesChanged(const QString& interface, const QVariantMap& changed, const QStringList& invalidated);

private:
    void apply(const QVariantMap& properties);

    Connectivity m_connectivity = Connectivity::Unknown;
    bool m_metered = false;
};

}  // namespace noctara::net
