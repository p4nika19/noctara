#pragma once

#include "core/Error.hpp"
#include "theme/UserTheme.hpp"

#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>

#include <functional>
#include <memory>
#include <vector>

namespace noctara::settings {
class SettingsStore;
}

namespace noctara::theme {

struct ThemeEntry {
    QString id;  // file name without .toml; what the "custom theme" setting stores
    QString name;
    QString description;
    QString author;
    QString file;
    std::vector<ThemeIssue> issues;
    std::shared_ptr<const UserTheme> theme;  // null when the file could not be used (syntax error, unreadable)
};

// The user themes in <config>/themes/*.toml. The constructor reads the folder once, so the selected theme is
// active before the first frame; later scans parse off the GUI thread when files change (hot reload). The one named
// by appearance.theme.custom is active. Themes are data only: nothing in them is ever executed. GUI thread.
class ThemeLibrary : public QObject {
    Q_OBJECT

public:
    ThemeLibrary(settings::SettingsStore& settings, QString directory, QObject* parent = nullptr);

    [[nodiscard]] const QString& directory() const noexcept { return m_directory; }
    [[nodiscard]] const std::vector<ThemeEntry>& themes() const noexcept { return m_themes; }
    [[nodiscard]] bool scanned() const noexcept { return m_scanned; }
    // The selected theme, or null for the built-in look (none selected, missing or unusable).
    [[nodiscard]] std::shared_ptr<const UserTheme> active() const noexcept { return m_active; }
    // The selected theme's entry, if its file exists.
    [[nodiscard]] const ThemeEntry* selectedEntry() const;
    [[nodiscard]] QString selectedId() const;

    // Reads the folder again (also done automatically when it changes).
    void rescan();
    // Copies a theme file into the folder after checking it parses; delivers the new id on `context`'s thread
    // (which must be the library's) once the folder has been scanned again, so themes() already lists it.
    void importFile(const QString& path, QObject* context, std::function<void(Result<QString>)> done);
    // Copies a theme's file to `path`.
    void exportTheme(const QString& id, const QString& path, QObject* context, std::function<void(Result<void>)> done);

    static constexpr int kMaxThemes = 200;
    static constexpr qint64 kMaxFileBytes = 256 * 1024;

Q_SIGNALS:
    void themesChanged();
    void activeChanged();

private:
    void applyScan(std::vector<ThemeEntry> themes, quint64 generation);
    void updateActive();
    void watch();

    settings::SettingsStore& m_settings;
    QString m_directory;
    std::vector<ThemeEntry> m_themes;
    std::shared_ptr<const UserTheme> m_active;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    std::vector<std::function<void()>> m_afterScan;  // run once the next current scan is applied
    quint64 m_generation = 0;
    bool m_scanned = false;
};

// Reads and parses every theme file of `directory` (blocking; used off the GUI thread and in tests).
[[nodiscard]] std::vector<ThemeEntry> scanThemes(const QString& directory);

}  // namespace noctara::theme
