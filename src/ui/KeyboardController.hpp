#pragma once

#include "ui/GlobalShortcuts.hpp"

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

namespace noctara::settings {
class SettingsStore;
}
namespace noctara::i18n {
class LanguageManager;
}

namespace noctara::ui {

class SettingsController;

// QML "Shortcuts" singleton: the remappable keyboard shortcuts (keyboard.shortcuts.<action>), their conflicts
// (shown on the settings rows), the cheat sheet, the key recorder's conversion, and the global shortcuts that
// the desktop portal provides.
class KeyboardController : public QObject {
    Q_OBJECT
    // action → QKeySequence portable text ("" = none). QML Shortcut items bind to these.
    Q_PROPERTY(QVariantMap sequences READ sequences NOTIFY changed)
    // [{action, label, keys}] in settings order; keys in the platform's own notation.
    Q_PROPERTY(QVariantList cheatSheet READ cheatSheet NOTIFY changed)

public:
    KeyboardController(settings::SettingsStore& settings, i18n::LanguageManager& languages,
                       SettingsController& settingsUi, QObject* parent = nullptr);

    [[nodiscard]] QVariantMap sequences() const;
    [[nodiscard]] QVariantList cheatSheet() const;

    // The shortcut text for a key press (Qt::Key + modifiers); "" for a lone modifier.
    Q_INVOKABLE [[nodiscard]] static QString fromKeyEvent(int key, int modifiers);
    // Portable text → how the platform writes it ("Ctrl+Shift+Q" → "Ctrl+Shift+Q", "Meta" → "Super" etc.).
    Q_INVOKABLE [[nodiscard]] static QString display(const QString& portable);
    // Actions sharing a sequence: action → labels of the others.
    [[nodiscard]] QVariantMap conflicts() const;

    static constexpr std::string_view kPrefix = "keyboard.shortcuts.";

Q_SIGNALS:
    void changed();
    // A global (portal) shortcut fired: "play_pause", "next", "previous", "show".
    void globalActivated(const QString& action);

private:
    void refresh();
    void applyGlobal();

    settings::SettingsStore& m_settings;
    SettingsController& m_settingsUi;
    GlobalShortcuts m_global;
};

}  // namespace noctara::ui
