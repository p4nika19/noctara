#include "cache/DiskLru.hpp"

#include "cache/FileOps.hpp"
#include "core/FileIo.hpp"
#include "core/Log.hpp"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

#include <atomic>

namespace noctara::cache {

namespace {

constexpr auto kIndexName = "index";
constexpr QByteArrayView kIndexHeader = "noctara-lru 1\n";

bool validName(QByteArrayView name)
{
    return name.size() >= 8 &&
           std::ranges::all_of(name, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

}  // namespace

std::int64_t unixNow()
{
    return QDateTime::currentSecsSinceEpoch();
}

std::int64_t accessStamp()
{
    static std::atomic<std::int64_t> last{0};
    std::int64_t now = QDateTime::currentMSecsSinceEpoch();
    std::int64_t previous = last.load();
    while (!last.compare_exchange_weak(previous, std::max(now, previous + 1))) {
    }
    return std::max(now, previous + 1);
}

DiskLru::DiskLru(QString directory, std::int64_t budget, FileOps& ops)
    : m_directory(std::move(directory))
    , m_budget(budget)
    , m_ops(ops)
{
}

QString DiskLru::pathFor(const QByteArray& name) const
{
    return m_directory + u'/' + QString::fromLatin1(name.first(2)) + u'/' + QString::fromLatin1(name);
}

void DiskLru::load()
{
    if (m_loaded) {
        return;
    }
    m_loaded = true;
    const Result<QByteArray> index = readWholeFile(m_directory + u'/' + QLatin1StringView(kIndexName));
    if (!index || !index->startsWith(kIndexHeader)) {
        rebuild();
        return;
    }
    for (const QByteArray& line : index->sliced(kIndexHeader.size()).split('\n')) {
        const QList<QByteArray> fields = line.split(' ');
        if (fields.size() == 3 && validName(fields[0])) {
            m_ledger.upsert(fields[0], fields[1].toLongLong(), fields[2].toLongLong());
        }
    }
}

void DiskLru::rebuild()
{
    m_ledger.clear();
    QDirIterator it(m_directory, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo info = it.nextFileInfo();
        const QByteArray name = info.fileName().toLatin1();
        if (validName(name) && info.dir().dirName().toLatin1() == name.first(2)) {
            m_ledger.upsert(name, info.size(), info.lastModified().toMSecsSinceEpoch());
        } else if (info.fileName().endsWith(u".tmp")) {
            QFile::remove(info.filePath());  // left over from an interrupted write
        }
    }
    m_dirty = true;
    evict();
}

Result<QByteArray> DiskLru::get(const QByteArray& name)
{
    load();
    if (!m_ledger.contains(name)) {
        return fail(ErrorCode::NotFound, "not cached");
    }
    Result<QByteArray> data = readWholeFile(pathFor(name));
    if (!data) {
        m_ledger.remove(name);  // deleted behind our back: a miss
        m_dirty = true;
        return data;
    }
    m_ledger.touch(name, accessStamp());
    m_dirty = true;
    return data;
}

Result<void> DiskLru::put(const QByteArray& name, QByteArrayView data)
{
    load();
    if (m_budget <= 0 || data.size() > m_budget) {
        return fail(ErrorCode::NoSpace, "larger than the tier's budget");
    }
    if (auto dir = ensureDirectory(m_directory + u'/' + QString::fromLatin1(name.first(2))); !dir) {
        return dir;
    }
    Result<void> written = m_ops.writeFile(pathFor(name), data);
    if (!written && written.error().code == ErrorCode::NoSpace && m_ledger.size() > 0) {
        // Disk full: make room from our own files, then try once more.
        for (const QByteArray& victim : m_ledger.victims(std::max<std::int64_t>(data.size(), m_budget / 10), name)) {
            remove(victim);
        }
        written = m_ops.writeFile(pathFor(name), data);
    }
    if (!written) {
        return written;
    }
    m_ledger.upsert(name, data.size(), accessStamp());
    m_dirty = true;
    evict();
    return {};
}

void DiskLru::remove(const QByteArray& name)
{
    QFile::remove(pathFor(name));
    m_ledger.remove(name);
    m_dirty = true;
}

void DiskLru::clear()
{
    for (const QString& entry : QDir(m_directory).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        removeTree(m_directory + u'/' + entry);
    }
    QFile::remove(m_directory + u'/' + QLatin1StringView(kIndexName));
    m_ledger.clear();
    m_dirty = false;
}

void DiskLru::setBudget(std::int64_t bytes)
{
    m_budget = bytes;
    if (m_loaded) {
        evict();
    }
}

void DiskLru::evict()
{
    for (const QByteArray& victim : m_ledger.overBudget(std::max<std::int64_t>(0, m_budget))) {
        remove(victim);
    }
}

void DiskLru::flushIndex()
{
    if (!m_dirty || !m_loaded) {
        return;
    }
    QByteArray index = kIndexHeader.toByteArray();
    for (auto it = m_ledger.items().cbegin(); it != m_ledger.items().cend(); ++it) {
        index += it.key() + ' ' + QByteArray::number(it->bytes) + ' ' + QByteArray::number(it->lastAccess) + '\n';
    }
    if (auto written = m_ops.writeFile(m_directory + u'/' + QLatin1StringView(kIndexName), index); !written) {
        qCDebug(lcCache) << "could not write the index of" << m_directory << written.error().describe();
        return;
    }
    m_dirty = false;
}

}  // namespace noctara::cache
