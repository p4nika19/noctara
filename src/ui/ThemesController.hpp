#pragma once

#include <QObject>
#include <QUrl>
#include <QVariantList>

namespace noctara::settings {
class SettingsStore;
}
namespace noctara::i18n {
class LanguageManager;
}
namespace noctara::theme {
class ThemeLibrary;
}

namespace noctara::ui {

// QML "Themes" singleton: the user theme files for the "Custom theme" picker, with their problems as translated
// messages, and import, export and "open folder".
class ThemesController : public QObject {
    Q_OBJECT
    // [{id, name, description, author, usable, issues: [message…]}]
    Q_PROPERTY(QVariantList themes READ themes NOTIFY themesChanged)
    Q_PROPERTY(QString selected READ selected NOTIFY themesChanged)
    // The selected theme's problems, or that it is missing.
    Q_PROPERTY(QStringList selectedIssues READ selectedIssues NOTIFY themesChanged)
    Q_PROPERTY(QString directory READ directory CONSTANT)

public:
    ThemesController(theme::ThemeLibrary& library, settings::SettingsStore& settings, i18n::LanguageManager& languages,
                     QObject* parent = nullptr);

    [[nodiscard]] QVariantList themes() const;
    [[nodiscard]] QString selected() const;
    [[nodiscard]] QStringList selectedIssues() const;
    [[nodiscard]] QString directory() const;

    // "" selects the built-in look.
    Q_INVOKABLE void select(const QString& id);
    Q_INVOKABLE void importFile(const QUrl& file);
    Q_INVOKABLE void exportTheme(const QString& id, const QUrl& file);
    Q_INVOKABLE void openFolder();
    Q_INVOKABLE void reload();

Q_SIGNALS:
    void themesChanged();
    // kind: "info" | "warning" | "error"
    void notice(const QString& kind, const QString& message);

private:
    theme::ThemeLibrary& m_library;
    settings::SettingsStore& m_settings;
};

}  // namespace noctara::ui
