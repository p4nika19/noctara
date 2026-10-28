#include "cache/FileOps.hpp"

#include "core/FileIo.hpp"
#include "core/UniqueFd.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <format>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <system_error>
#include <unistd.h>

namespace noctara::cache {

namespace {

Result<void> errnoFailure(std::string_view what, int err)
{
    return fail(errorCodeFromErrno(err), std::format("{}: {}", what, std::generic_category().message(err)));
}

}  // namespace

Result<void> FileOps::writeAt(int fd, QByteArrayView data, std::int64_t offset)
{
    const char* cursor = data.data();
    auto remaining = static_cast<std::size_t>(data.size());
    auto position = static_cast<off_t>(offset);
    while (remaining > 0) {
        const ssize_t written = ::pwrite(fd, cursor, remaining, position);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errnoFailure("pwrite", errno);
        }
        cursor += written;
        remaining -= static_cast<std::size_t>(written);
        position += written;
    }
    return {};
}

Result<void> FileOps::writeFile(const QString& path, QByteArrayView data)
{
    return writeFileAtomic(path, data, 0600, Durability::Atomic);
}

Result<void> FileOps::copyFile(const QString& from, const QString& to)
{
    const UniqueFd in(::open(QFile::encodeName(from).constData(), O_RDONLY | O_CLOEXEC));
    if (!in) {
        return errnoFailure("open", errno);
    }
    const UniqueFd out(::open(QFile::encodeName(to).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
    if (!out) {
        return errnoFailure("create", errno);
    }
    std::array<char, std::size_t{64} * 1024> buffer{};
    std::int64_t offset = 0;
    while (true) {
        const ssize_t got = ::read(in.get(), buffer.data(), buffer.size());
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errnoFailure("read", errno);
        }
        if (got == 0) {
            return {};
        }
        if (auto written = writeAt(out.get(), QByteArrayView(buffer.data(), got), offset); !written) {
            return written;
        }
        offset += got;
    }
}

FileOps& FileOps::posix()
{
    static FileOps instance;
    return instance;
}

Result<QByteArray> readAt(int fd, std::int64_t offset, std::int64_t size)
{
    QByteArray data(static_cast<qsizetype>(size), Qt::Uninitialized);
    qsizetype done = 0;
    while (done < data.size()) {
        const ssize_t got = ::pread(fd, data.data() + done, static_cast<std::size_t>(data.size() - done),
                                    static_cast<off_t>(offset + done));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(errnoFailure("pread", errno).error());
        }
        if (got == 0) {
            break;
        }
        done += got;
    }
    data.truncate(done);
    return data;
}

std::int64_t freeDiskSpace(const QString& path)
{
    struct statvfs info{};
    if (::statvfs(QFile::encodeName(path).constData(), &info) != 0) {
        return -1;
    }
    return static_cast<std::int64_t>(info.f_bavail) * static_cast<std::int64_t>(info.f_frsize);
}

std::int64_t directorySize(const QString& path)
{
    std::int64_t total = 0;
    QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        // Allocated blocks, not the apparent size: partial audio entries are sparse files.
        struct stat info{};
        if (::lstat(QFile::encodeName(it.next()).constData(), &info) == 0) {
            total += static_cast<std::int64_t>(info.st_blocks) * 512;
        }
    }
    return total;
}

bool removeTree(const QString& path)
{
    return QDir(path).removeRecursively();
}

}  // namespace noctara::cache
