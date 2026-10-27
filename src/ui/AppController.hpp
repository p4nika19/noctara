#pragma once

#include "settings/ConfigIssue.hpp"

#include <QObject>
#include <QString>

namespace noctara::settings {
class SettingsStore;
}
namespace noctara::gfx {
class GraphicsInfo;
class RelaunchGuard;
}  // namespace noctara::gfx
namespace noctara::i18n {
class LanguageManager;
}

namespace noctara::ui {

// QML "App" singleton: application-level actions (restart), toasts and one-time notices.
class AppController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString graphicsNotice READ graphicsNotice NOTIFY graphicsNoticeChanged)

public:
    AppController(settings::SettingsStore& settings, gfx::GraphicsInfo& graphics, gfx::RelaunchGuard& relaunch,
                  i18n::LanguageManager& languages, QObject* parent = nullptr);

    [[nodiscard]] QString version() const;
    [[nodiscard]] QString graphicsNotice() const;

    Q_INVOKABLE void restart();
    Q_INVOKABLE void triggerAction(const QString& actionId);
    Q_INVOKABLE void dismissGraphicsNotice();
    // Bundled license texts shown in About (allow-listed ids: "manrope", "inter", "jetbrains-mono", "lucide").
    Q_INVOKABLE QString licenseText(const QString& id) const;

    void reportConfigIssues(const settings::ConfigIssues& issues);
    Q_INVOKABLE void showToast(const QString& kind, const QString& message, const QString& actionLabel = {},
                               const QString& actionId = {});

Q_SIGNALS:
    // kind: "info" | "success" | "warning" | "error"
    void toastRequested(const QString& kind, const QString& message, const QString& actionLabel,
                        const QString& actionId);
    void graphicsNoticeChanged();

private:
    void onGraphicsFailed();

    settings::SettingsStore& m_settings;
    gfx::GraphicsInfo& m_graphics;
    gfx::RelaunchGuard& m_relaunch;
};

}  // namespace noctara::ui
