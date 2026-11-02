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
    // Puts text on the clipboard (links from context menus).
    Q_INVOKABLE static void copyText(const QString& text);
    // Opens a web link in the default browser.
    Q_INVOKABLE static void openLink(const QString& url);
    // Bundled license texts shown in About (allow-listed ids: "manrope", "inter", "jetbrains-mono", "lucide").
    Q_INVOKABLE QString licenseText(const QString& id) const;

    void reportConfigIssues(const settings::ConfigIssues& issues);
    void reportConfigSaveFailed(const Error& error);
    Q_INVOKABLE void showToast(const QString& kind, const QString& message, const QString& actionLabel = {},
                               const QString& actionId = {});

Q_SIGNALS:
    // kind: "info" | "success" | "warning" | "error"
    void toastRequested(const QString& kind, const QString& message, const QString& actionLabel,
                        const QString& actionId);
    // Main.qml switches to `page` (and, for "settings", to the settings `category`).
    void pageRequested(const QString& page, const QString& category);
    void graphicsNoticeChanged();

private:
    void onGraphicsFailed();

    settings::SettingsStore& m_settings;
    gfx::GraphicsInfo& m_graphics;
    gfx::RelaunchGuard& m_relaunch;
};

}  // namespace noctara::ui
