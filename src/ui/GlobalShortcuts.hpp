#pragma once

#include <QDBusObjectPath>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <functional>
#include <vector>

namespace noctara::ui {

// One shortcut as the portal sees it: an id and its properties ("description", "preferred_trigger").
struct PortalShortcut {
    QString id;
    QVariantMap properties;
};

// Global shortcuts through the XDG desktop portal (org.freedesktop.portal.GlobalShortcuts): Noctara registers
// its actions, the desktop lets the user pick the keys and reports activations even while the window is in the
// background. Where the portal is missing nothing happens (media keys keep working through MPRIS).
class GlobalShortcuts : public QObject {
    Q_OBJECT

public:
    struct Action {
        QString id;
        QString description;  // translated
    };

    explicit GlobalShortcuts(QObject* parent = nullptr);
    ~GlobalShortcuts() override;
    GlobalShortcuts(const GlobalShortcuts&) = delete;
    GlobalShortcuts& operator=(const GlobalShortcuts&) = delete;
    GlobalShortcuts(GlobalShortcuts&&) = delete;
    GlobalShortcuts& operator=(GlobalShortcuts&&) = delete;

    // Opens a portal session and binds `actions` (once per session); false closes the session.
    void setEnabled(bool enabled, std::vector<Action> actions);
    [[nodiscard]] bool active() const noexcept { return !m_session.isEmpty(); }
    [[nodiscard]] static bool available();

Q_SIGNALS:
    void activated(const QString& id);

private Q_SLOTS:
    void onSessionCreated(uint response, const QVariantMap& results);
    void onBound(uint response, const QVariantMap& results);
    void onActivated(const QDBusObjectPath& session, const QString& id, qulonglong timestamp,
                     const QVariantMap& options);

private:
    [[nodiscard]] QString requestPath(const QString& token) const;
    void closeSession();

    std::vector<Action> m_actions;
    QString m_session;  // object path of the open session
    QString m_pending;  // request path we wait on
    bool m_enabled = false;
    int m_counter = 0;
};

}  // namespace noctara::ui

Q_DECLARE_METATYPE(noctara::ui::PortalShortcut)
