#pragma once

#include "settings/ConfigIssue.hpp"

#include <QObject>
#include <QString>

#include <functional>

class QImage;
class QUrl;

namespace noctara::cache {
class ImageCache;
}
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
    void setImageCache(cache::ImageCache& images) noexcept { m_images = &images; }
    // Loads the original decoded artwork through the image cache, then copies or saves it.
    Q_INVOKABLE void copyArtwork(const QString& url);
    Q_INVOKABLE void saveArtwork(const QString& url, const QUrl& destination);
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
    void requestArtwork(const QString& url, std::function<void(const QImage&)> done);

    settings::SettingsStore& m_settings;
    gfx::GraphicsInfo& m_graphics;
    gfx::RelaunchGuard& m_relaunch;
    cache::ImageCache* m_images = nullptr;
};

}  // namespace noctara::ui
