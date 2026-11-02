#pragma once

#include "core/Error.hpp"
#include "settings/ConfigIssue.hpp"
#include "settings/SettingsStore.hpp"
#include "settings/TomlCodec.hpp"

#include <QFileSystemWatcher>
#include <QObject>
#include <QThreadPool>
#include <QTimer>

#include <memory>
#include <string>

namespace noctara::settings {

// Binds the user (and system) layers of a SettingsStore to TOML files: initial load, debounced atomic
// saves on a background thread, and validated hot reload of external edits.
class ConfigFile : public QObject {
    Q_OBJECT

public:
    ConfigFile(SettingsStore& store, QString userPath, QString systemPath, QObject* parent = nullptr);
    ~ConfigFile() override;

    ConfigFile(const ConfigFile&) = delete;
    ConfigFile& operator=(const ConfigFile&) = delete;

    // Synchronous: used once at startup, before the UI exists. Writes a documented template on first run.
    ConfigIssues load();

    void setHotReloadEnabled(bool enabled);
    void setSaveDelay(std::chrono::milliseconds delay);

    // Writes pending changes (and retries a save that failed) and waits for all background I/O to finish.
    void flush();

    // Moves the current file aside (<name>.bak) and writes a fresh template; resets the user layer.
    [[nodiscard]] Result<void> resetToDefaults();

    // Replaces the user layer from another file (profile import). Invalid files are rejected as a whole.
    [[nodiscard]] Result<ConfigIssues> importFrom(const QString& path);
    [[nodiscard]] Result<void> exportTo(const QString& path) const;

    [[nodiscard]] const QString& path() const noexcept { return m_userPath; }

Q_SIGNALS:
    // Problems found while hot-reloading external edits (the last good state stays active).
    void reloadIssues(const noctara::settings::ConfigIssues& issues);
    void reloaded();
    void saveFailed(const noctara::Error& error);

private:
    void scheduleSave();
    void saveNow();
    void onWatchedPathChanged();
    void reloadFromDisk();
    void applyReloaded(const ParsedConfig& parsed);
    void rewatch();
    [[nodiscard]] QByteArray render() const;

    SettingsStore& m_store;
    QString m_userPath;
    QString m_systemPath;
    QTimer m_saveTimer;
    QTimer m_reloadTimer;
    std::unique_ptr<QFileSystemWatcher> m_watcher;
    QThreadPool m_io;          // single thread: writes are applied in order
    QByteArray m_lastContent;  // last content written or read, to ignore our own writes
    std::string m_unknownToml;
    std::int64_t m_fileVersion = 0;
    bool m_fileUnparseable = false;  // back the file up before the first overwrite
    bool m_retrySave = false;        // the last save failed: flush() writes the state again
};

}  // namespace noctara::settings
