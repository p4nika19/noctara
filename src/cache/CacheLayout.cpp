#include "cache/CacheLayout.hpp"

#include "cache/FileOps.hpp"
#include "core/FileIo.hpp"
#include "core/Log.hpp"

#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace noctara::cache {

namespace {

constexpr auto kCacheDirTag = "Signature: 8a477f597d28d172789f06886806bc55\n"
                              "# This file is a cache directory tag created by Noctara.\n"
                              "# For information about cache directory tags see https://bford.info/cachedir/\n";

}  // namespace

Result<CacheLayout> prepareLayout(const QString& root)
{
    CacheLayout layout{root};
    if (auto created = ensureDirectory(root); !created) {
        return std::unexpected(created.error());
    }
    if (!QFileInfo(root).isDir() || ::access(QFile::encodeName(root).constData(), W_OK | X_OK) != 0) {
        return fail(ErrorCode::PermissionDenied, "cache directory is not writable");
    }
    const QByteArray expected = QByteArray::number(CacheLayout::kVersion);
    const Result<QByteArray> marker = readWholeFile(layout.versionFile());
    if (!marker || marker->trimmed() != expected) {
        // Only a marker of another layout version proves the subdirectories are ours to delete.
        if (marker) {
            qCInfo(lcCache) << "cache layout" << marker->trimmed() << "is not" << expected << "- clearing";
            for (const QString& dir : {layout.audio(), layout.images(), layout.http()}) {
                removeTree(dir);
            }
        }
        if (auto written = writeFileAtomic(layout.versionFile(), QByteArray(expected + '\n'), 0600, Durability::Atomic);
            !written) {
            return std::unexpected(written.error());
        }
    }
    for (const QString& dir : {layout.audio(), layout.covers(), layout.images(), layout.http(), layout.electron()}) {
        if (auto created = ensureDirectory(dir); !created) {
            return std::unexpected(created.error());
        }
    }
    const QString tag = root + QStringLiteral("/CACHEDIR.TAG");
    if (!QFileInfo::exists(tag)) {
        if (auto written = writeFileAtomic(tag, kCacheDirTag, 0600, Durability::Atomic); !written) {
            qCDebug(lcCache) << "CACHEDIR.TAG not written:" << written.error().describe();
        }
    }
    return layout;
}

Result<CacheLock> CacheLock::acquire(const CacheLayout& layout)
{
    UniqueFd fd(::open(QFile::encodeName(layout.lockFile()).constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
    if (!fd) {
        return fail(errorCodeFromErrno(errno), "cannot open the cache lock file");
    }
    if (::flock(fd.get(), LOCK_EX | LOCK_NB) != 0) {
        return fail(errno == EWOULDBLOCK ? ErrorCode::CacheLocked : errorCodeFromErrno(errno),
                    "cache directory is locked by another instance");
    }
    return CacheLock(std::move(fd));
}

}  // namespace noctara::cache
