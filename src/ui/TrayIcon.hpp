#pragma once

#include <QObject>

#include <memory>

class QAction;
class QDBusServiceWatcher;
class QMenu;
class QSystemTrayIcon;

namespace noctara::settings {
class SettingsStore;
}
namespace noctara::i18n {
class LanguageManager;
}
namespace noctara::media {
class PlaybackController;
}

namespace noctara::ui {

// Playback controls in the system tray (StatusNotifierItem, where the desktop shows one). Created only while the
// "tray icon" setting is on and a tray is available; the menu is exported to the desktop over D-Bus and follows
// the language live. Needs a QApplication (the menu is a QMenu). The desktop's tray host may start after Noctara
// (a bar launched later at login) or restart: the icon appears when the host does, and goes when it goes.
class TrayIcon : public QObject {
    Q_OBJECT
    // The icon is in the tray right now (closing the window can then hide it there).
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)

public:
    TrayIcon(media::PlaybackController& player, settings::SettingsStore& settings, i18n::LanguageManager& languages,
             QObject* parent = nullptr);
    ~TrayIcon() override;
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;
    TrayIcon(TrayIcon&&) = delete;
    TrayIcon& operator=(TrayIcon&&) = delete;

    [[nodiscard]] bool active() const noexcept { return m_icon != nullptr; }

Q_SIGNALS:
    // The icon was clicked or "Show Noctara" chosen.
    void showRequested();
    void activeChanged();
    // The tray host went away while the icon was in it: a window hidden in the tray must come back.
    void lost();

private:
    void apply(bool hostPresent);
    void create();
    void retranslate();
    void refresh();

    media::PlaybackController& m_player;
    settings::SettingsStore& m_settings;
    std::unique_ptr<QMenu> m_menu;
    std::unique_ptr<QSystemTrayIcon> m_icon;
    QDBusServiceWatcher* m_host = nullptr;  // the StatusNotifierWatcher (child of this)
    QAction* m_playPause = nullptr;         // owned by m_menu
    QAction* m_previous = nullptr;
    QAction* m_next = nullptr;
    QAction* m_show = nullptr;
    QAction* m_quit = nullptr;
};

}  // namespace noctara::ui
