#pragma once

#include "core/Error.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <cstdint>

namespace noctara::cache {

// The few file operations whose failures (ENOSPC above all) the cache must survive. Virtual so tests can make
// them fail; the default implementation is plain POSIX.
class FileOps {
public:
    FileOps() = default;
    virtual ~FileOps() = default;
    FileOps(const FileOps&) = delete;
    FileOps& operator=(const FileOps&) = delete;
    FileOps(FileOps&&) = delete;
    FileOps& operator=(FileOps&&) = delete;

    // Writes all of `data` at `offset`.
    [[nodiscard]] virtual Result<void> writeAt(int fd, QByteArrayView data, std::int64_t offset);
    // Atomic replace (temp file + rename), without fsync: caches re-validate what they read.
    [[nodiscard]] virtual Result<void> writeFile(const QString& path, QByteArrayView data);
    [[nodiscard]] virtual Result<void> copyFile(const QString& from, const QString& to);

    [[nodiscard]] static FileOps& posix();
};

// Reads exactly `size` bytes at `offset` (fewer only at the end of the file).
[[nodiscard]] Result<QByteArray> readAt(int fd, std::int64_t offset, std::int64_t size);
// Free bytes for unprivileged users on the file system holding `path`, -1 if unknown.
[[nodiscard]] std::int64_t freeDiskSpace(const QString& path);
// Disk space used by the regular files below `path`.
[[nodiscard]] std::int64_t directorySize(const QString& path);
bool removeTree(const QString& path);  // best effort: a leftover cache file is harmless

}  // namespace noctara::cache
