#include "settings/ConfigFile.hpp"

#include "core/FileIo.hpp"
#include "core/Log.hpp"

#include <QFile>
#include <QFileInfo>
#include <QtConcurrentRun>

namespace noctara::settings {

namespace {

constexpr auto kDefaultSaveDelay = std::chrono::milliseconds(400);
constexpr auto kReloadDebounce = std::chrono::milliseconds(150);

struct LoadedFile {
    QByteArray content;
    ParsedConfig parsed;
    bool exists = false;
};

LoadedFile loadFile(const QString& path, const Schema& schema)
{
    LoadedFile loaded;
    auto content = readWholeFile(path);
    if (!content) {
        if (content.error().code != ErrorCode::NotFound) {
            loaded.exists = true;
            loaded.parsed.syntaxOk = false;
            loaded.parsed.issues.append(ConfigIssue{ConfigIssue::Severity::Error,
                                                    content.error().code,
                                                    path,
                                                    0,
                                                    0,
                                                    {},
                                                    QString::fromStdString(content.error().detail)});
        }
        return loaded;
    }
    loaded.exists = true;
    loaded.content = std::move(*content);
    loaded.parsed = parseConfig(
        std::string_view(loaded.content.constData(), static_cast<std::size_t>(loaded.content.size())), path, schema);
    return loaded;
}

void logIssues(const ConfigIssues& issues)
{
    for (const ConfigIssue& issue : issues) {
        if (issue.severity == ConfigIssue::Severity::Error) {
            qCWarning(lcSettings).noquote() << issue.logText();
        } else {
            qCInfo(lcSettings).noquote() << issue.logText();
        }
    }
}

bool hasErrors(const ConfigIssues& issues)
{
    return std::ranges::any_of(issues,
                               [](const ConfigIssue& issue) { return issue.severity == ConfigIssue::Severity::Error; });
}

}  // namespace

ConfigFile::ConfigFile(SettingsStore& store, QString userPath, QString systemPath, QObject* parent)
    : QObject(parent)
    , m_store(store)
    , m_userPath(std::move(userPath))
    , m_systemPath(std::move(systemPath))
{
    m_io.setMaxThreadCount(1);
    m_io.setExpiryTimeout(-1);  // one long-lived I/O thread instead of churning threads per save
    m_io.setObjectName(QStringLiteral("config-io"));
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(kDefaultSaveDelay);
    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(kReloadDebounce);
    connect(&m_saveTimer, &QTimer::timeout, this, &ConfigFile::saveNow);
    connect(&m_reloadTimer, &QTimer::timeout, this, &ConfigFile::reloadFromDisk);
    connect(&m_store, &SettingsStore::userLayerEdited, this, &ConfigFile::scheduleSave);
}

ConfigFile::~ConfigFile()
{
    flush();
}

ConfigIssues ConfigFile::load()
{
    ConfigIssues issues;
    if (!m_systemPath.isEmpty()) {
        const LoadedFile system = loadFile(m_systemPath, m_store.schema());
        issues.append(system.parsed.issues);
        m_store.replaceLayer(Layer::System, system.parsed.values);
    }

    const LoadedFile user = loadFile(m_userPath, m_store.schema());
    issues.append(user.parsed.issues);
    logIssues(issues);
    if (!user.exists) {
        qCInfo(lcSettings).noquote() << "writing documented config template to" << m_userPath;
        m_lastContent = render();
        m_io.start([path = m_userPath, content = m_lastContent] {
            if (auto written = writeFileAtomic(path, content); !written) {
                qCWarning(lcSettings).noquote() << QString::fromStdString(written.error().describe());
            }
        });
        return issues;
    }

    m_lastContent = user.content;
    m_fileUnparseable = !user.parsed.syntaxOk;
    m_unknownToml = user.parsed.unknownToml;
    m_fileVersion = user.parsed.version;
    m_store.replaceLayer(Layer::User, user.parsed.values);
    return issues;
}

void ConfigFile::setSaveDelay(std::chrono::milliseconds delay)
{
    m_saveTimer.setInterval(delay);
}

void ConfigFile::setHotReloadEnabled(bool enabled)
{
    if (!enabled) {
        m_watcher.reset();
        return;
    }
    if (m_watcher) {
        return;
    }
    if (auto dir = ensureDirectory(QFileInfo(m_userPath).absolutePath()); !dir) {
        qCWarning(lcSettings).noquote() << "hot reload disabled:" << QString::fromStdString(dir.error().describe());
        return;
    }
    m_watcher = std::make_unique<QFileSystemWatcher>();
    connect(m_watcher.get(), &QFileSystemWatcher::fileChanged, this, &ConfigFile::onWatchedPathChanged);
    connect(m_watcher.get(), &QFileSystemWatcher::directoryChanged, this, &ConfigFile::onWatchedPathChanged);
    m_watcher->addPath(QFileInfo(m_userPath).absolutePath());
    rewatch();
}

void ConfigFile::rewatch()
{
    // Atomic replacement (ours or an editor's) drops the inode watch; re-arm it when the file exists.
    if (m_watcher && QFileInfo::exists(m_userPath) && !m_watcher->files().contains(m_userPath)) {
        m_watcher->addPath(m_userPath);
    }
}

void ConfigFile::onWatchedPathChanged()
{
    rewatch();
    m_reloadTimer.start();
}

void ConfigFile::reloadFromDisk()
{
    QtConcurrent::run(&m_io, [path = m_userPath, schema = &m_store.schema()] {
        return loadFile(path, *schema);
    }).then(this, [this](const LoadedFile& loaded) {
        if (!loaded.exists || loaded.content == m_lastContent) {
            return;  // deleted (keep state) or our own write
        }
        m_lastContent = loaded.content;
        logIssues(loaded.parsed.issues);
        if (!loaded.parsed.syntaxOk) {
            m_fileUnparseable = true;  // the next save keeps this hand edit as config.toml.bak
            Q_EMIT reloadIssues(loaded.parsed.issues);
            return;
        }
        applyReloaded(loaded.parsed);
    });
}

void ConfigFile::applyReloaded(const ParsedConfig& parsed)
{
    // Invalid keys keep their last good value instead of silently reverting to defaults.
    ValueMap merged = parsed.values;
    for (const ConfigIssue& issue : parsed.issues) {
        if (issue.severity != ConfigIssue::Severity::Error) {
            continue;
        }
        if (const auto index = m_store.schema().indexOf(issue.key.toStdString())) {
            if (auto previous = m_store.layerValue(Layer::User, *index)) {
                merged.emplace_back(*index, std::move(*previous));
            }
        }
    }
    m_unknownToml = parsed.unknownToml;
    m_fileVersion = parsed.version;
    m_fileUnparseable = false;
    m_store.replaceLayer(Layer::User, merged);
    if (hasErrors(parsed.issues)) {
        Q_EMIT reloadIssues(parsed.issues);
    }
    Q_EMIT reloaded();
}

QByteArray ConfigFile::render() const
{
    return serializeConfig(m_store.schema(), m_store.layerSnapshot(Layer::User), m_unknownToml,
                           std::max(m_fileVersion, Schema::kConfigVersion));
}

void ConfigFile::scheduleSave()
{
    m_saveTimer.start();
}

void ConfigFile::saveNow()
{
    m_saveTimer.stop();
    QByteArray content = render();
    if (content == m_lastContent) {
        return;
    }
    const bool backup = std::exchange(m_fileUnparseable, false);
    QByteArray previous = std::exchange(m_lastContent, content);
    m_retrySave = false;
    QtConcurrent::run(&m_io,
                      [path = m_userPath, content, backup] -> Result<void> {
                          if (backup) {
                              const QString backupPath = path + QStringLiteral(".bak");
                              QFile::remove(backupPath);
                              QFile::copy(path, backupPath);
                          }
                          return writeFileAtomic(path, content);
                      })
        .then(this, [this, attempted = std::move(content), previous = std::move(previous),
                     backup](const Result<void>& written) {
            if (written) {
                return;
            }
            qCWarning(lcSettings).noquote()
                << "saving config failed:" << QString::fromStdString(written.error().describe());
            if (m_lastContent == attempted) {
                // The file still holds the previous content: a hot reload must not take that for an external edit,
                // and the next save or flush writes the unsaved state again (backing a broken file up first).
                m_lastContent = previous;
                m_fileUnparseable = m_fileUnparseable || backup;
                m_retrySave = true;
            }
            Q_EMIT saveFailed(written.error());
        });
}

void ConfigFile::flush()
{
    if (m_saveTimer.isActive() || m_retrySave) {
        saveNow();
    }
    m_io.waitForDone();
}

Result<void> ConfigFile::resetToDefaults()
{
    flush();
    if (QFileInfo::exists(m_userPath)) {
        const QString backupPath = m_userPath + QStringLiteral(".bak");
        QFile::remove(backupPath);
        if (!QFile::rename(m_userPath, backupPath)) {
            return fail(ErrorCode::IoError, "could not move the existing config aside");
        }
    }
    m_unknownToml.clear();
    m_fileUnparseable = false;
    m_store.replaceLayer(Layer::User, {});
    m_lastContent = render();
    return writeFileAtomic(m_userPath, m_lastContent);
}

Result<ConfigIssues> ConfigFile::importFrom(const QString& path)
{
    LoadedFile loaded = loadFile(path, m_store.schema());
    if (!loaded.exists) {
        return fail(ErrorCode::NotFound, path.toStdString());
    }
    if (!loaded.parsed.syntaxOk) {
        return fail(ErrorCode::ConfigSyntaxError, loaded.parsed.issues.isEmpty()
                                                      ? std::string()
                                                      : loaded.parsed.issues.front().logText().toStdString());
    }
    m_unknownToml = loaded.parsed.unknownToml;
    m_store.replaceLayer(Layer::User, loaded.parsed.values);
    scheduleSave();
    return loaded.parsed.issues;
}

Result<void> ConfigFile::exportTo(const QString& path) const
{
    return writeFileAtomic(path, render(), 0644);
}

}  // namespace noctara::settings
