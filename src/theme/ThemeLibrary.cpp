#include "theme/ThemeLibrary.hpp"

#include "core/FileIo.hpp"
#include "core/Handoff.hpp"
#include "core/Log.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"

#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QRegularExpression>
#include <QtConcurrentRun>

#include <algorithm>
#include <utility>

namespace noctara::theme {

namespace keys = settings::keys;

namespace {

Result<QByteArray> readTheme(const QString& path)
{
    const QFileInfo info(path);
    if (!info.isFile()) {
        return fail(ErrorCode::NotFound, "not a file");
    }
    if (info.size() > ThemeLibrary::kMaxFileBytes) {
        return fail(ErrorCode::InvalidArgument, "theme file too large");
    }
    return readWholeFile(path);
}

// A file name for an imported theme: letters, digits, '-' and '_' only.
QString safeId(const QString& base)
{
    static const QRegularExpression unsafe(QStringLiteral("[^A-Za-z0-9_-]+"));
    QString id = base;
    id.replace(unsafe, QStringLiteral("-"));
    id = id.left(64);
    return id.isEmpty() ? QStringLiteral("theme") : id;
}

}  // namespace

std::vector<ThemeEntry> scanThemes(const QString& directory)
{
    std::vector<ThemeEntry> out;
    const QDir dir(directory);
    const QStringList files = dir.entryList({QStringLiteral("*.toml")}, QDir::Files | QDir::Readable, QDir::Name);
    for (const QString& file : files.mid(0, ThemeLibrary::kMaxThemes)) {
        ThemeEntry entry;
        entry.id = file.chopped(5);
        entry.name = entry.id;
        entry.file = dir.filePath(file);
        const Result<QByteArray> data = readTheme(entry.file);
        if (!data) {
            entry.issues.push_back({ThemeIssue::Kind::Syntax, {}, QString::fromStdString(data.error().describe()), 0});
            out.push_back(std::move(entry));
            continue;
        }
        Result<UserTheme> parsed = parseUserTheme(*data, entry.id, entry.issues);
        if (parsed) {
            entry.name = parsed->name;
            entry.description = parsed->description;
            entry.author = parsed->author;
            entry.theme = std::make_shared<const UserTheme>(std::move(*parsed));
        }
        out.push_back(std::move(entry));
    }
    return out;
}

ThemeLibrary::ThemeLibrary(settings::SettingsStore& settings, QString directory, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_directory(std::move(directory))
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(250);  // editors save in several steps
    connect(&m_debounce, &QTimer::timeout, this, &ThemeLibrary::rescan);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_debounce, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_debounce, qOverload<>(&QTimer::start));
    connect(&m_settings, &settings::SettingsStore::valueChanged, this, [this](const QString& key) {
        if (key == QLatin1StringView(keys::CustomTheme)) {
            updateActive();
        }
    });
    rescan();
}

const ThemeEntry* ThemeLibrary::selectedEntry() const
{
    const QString id = selectedId();
    const auto it = std::ranges::find(m_themes, id, &ThemeEntry::id);
    return id.isEmpty() || it == m_themes.end() ? nullptr : &*it;
}

QString ThemeLibrary::selectedId() const
{
    return m_settings.stringValue(keys::CustomTheme);
}

void ThemeLibrary::rescan()
{
    const quint64 generation = ++m_generation;
    (void)QtConcurrent::run([directory = m_directory] {
        // Created so it can be opened and watched; without it there is simply nothing to list.
        if (const Result<void> made = ensureDirectory(directory); !made) {
            qCInfo(lcTheme).noquote() << "no themes folder:" << QString::fromStdString(made.error().describe());
        }
        return scanThemes(directory);
    }).then(this, [this, generation](std::vector<ThemeEntry> themes) { applyScan(std::move(themes), generation); });
}

void ThemeLibrary::applyScan(std::vector<ThemeEntry> themes, quint64 generation)
{
    if (generation != m_generation) {
        return;  // a newer scan is on its way
    }
    for (const ThemeEntry& entry : themes) {
        for (const ThemeIssue& issue : entry.issues) {
            qCInfo(lcTheme).noquote() << "theme" << entry.file << "line" << issue.line << issue.key << ":"
                                      << issue.detail;
        }
    }
    m_themes = std::move(themes);
    m_scanned = true;
    watch();
    Q_EMIT themesChanged();
    updateActive();
    for (const std::function<void()>& callback : std::exchange(m_afterScan, {})) {
        callback();
    }
}

void ThemeLibrary::watch()
{
    if (!m_watcher.files().isEmpty()) {
        m_watcher.removePaths(m_watcher.files());
    }
    if (m_watcher.directories().isEmpty() && QFileInfo(m_directory).isDir()) {
        m_watcher.addPath(m_directory);
    }
    QStringList files;
    for (const ThemeEntry& entry : m_themes) {
        files.append(entry.file);
    }
    if (!files.isEmpty()) {
        (void)m_watcher.addPaths(files);
    }
}

void ThemeLibrary::updateActive()
{
    const ThemeEntry* entry = selectedEntry();
    std::shared_ptr<const UserTheme> next = entry != nullptr ? entry->theme : nullptr;
    if (next == m_active) {
        return;
    }
    if (!selectedId().isEmpty() && next == nullptr && m_scanned) {
        qCInfo(lcTheme).noquote() << "custom theme" << selectedId()
                                  << "is missing or unusable; using the built-in look";
    }
    m_active = std::move(next);
    Q_EMIT activeChanged();
}

void ThemeLibrary::importFile(const QString& path, QObject* context, std::function<void(Result<QString>)> done)
{
    (void)QtConcurrent::run([path, directory = m_directory] -> Result<QString> {
        Result<QByteArray> data = readTheme(path);
        if (!data) {
            return std::unexpected(data.error());
        }
        const QString base = safeId(QFileInfo(path).completeBaseName());
        std::vector<ThemeIssue> issues;
        if (Result<UserTheme> parsed = parseUserTheme(*data, base, issues); !parsed) {
            return std::unexpected(parsed.error());
        }
        if (Result<void> made = ensureDirectory(directory); !made) {
            return std::unexpected(made.error());
        }
        QString id = base;
        for (int n = 2; QFileInfo::exists(directory + u'/' + id + QStringLiteral(".toml")); ++n) {
            id = base + u'-' + QString::number(n);
        }
        if (Result<void> written = createFileAtomic(directory + u'/' + id + QStringLiteral(".toml"), *data, 0644);
            !written) {
            return std::unexpected(written.error());
        }
        return id;
    }).then(context, [this, guard = QPointer<QObject>(context), done = std::move(done)](Result<QString> result) {
        rescan();
        if (!result) {
            done(std::move(result));
            return;
        }
        // Delivered once the scan that lists the new file is applied, so selecting the id finds it.
        m_afterScan.emplace_back([guard, done, result = std::move(result)] mutable {
            if (guard) {
                done(std::move(result));
            }
        });
    });
}

void ThemeLibrary::exportTheme(const QString& id, const QString& path, QObject* context,
                               std::function<void(Result<void>)> done)
{
    const auto it = std::ranges::find(m_themes, id, &ThemeEntry::id);
    if (it == m_themes.end()) {
        done(fail(ErrorCode::NotFound, "no such theme"));
        return;
    }
    (void)QtConcurrent::run([source = it->file, path] -> Result<void> {
        Result<QByteArray> data = readTheme(source);
        if (!data) {
            return std::unexpected(data.error());
        }
        return writeFileAtomic(path, *data, 0644, Durability::Synced);
    }).then(context, [done = std::move(done)](Result<void> result) { done(std::move(result)); });
}

}  // namespace noctara::theme
