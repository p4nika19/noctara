#include "core/FileIo.hpp"

#include "core/UniqueFd.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>

#include <cerrno>
#include <fcntl.h>
#include <format>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace noctara {

namespace {

std::string errnoDetail(std::string_view what, const QString& path, int err)
{
    return std::format("{} '{}': {}", what, path.toStdString(), std::generic_category().message(err));
}

Result<void> writeAll(int fd, QByteArrayView data, const QString& path)
{
    const char* cursor = data.data();
    auto remaining = static_cast<std::size_t>(data.size());
    while (remaining > 0) {
        const ssize_t written = ::write(fd, cursor, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int err = errno;
            return fail(errorCodeFromErrno(err), errnoDetail("write", path, err));
        }
        cursor += written;
        remaining -= static_cast<std::size_t>(written);
    }
    return {};
}

}  // namespace

ErrorCode errorCodeFromErrno(int err) noexcept
{
    switch (err) {
    case ENOENT: return ErrorCode::NotFound;
    case EEXIST: return ErrorCode::AlreadyExists;
    case EACCES:
    case EPERM:
    case EROFS: return ErrorCode::PermissionDenied;
    case ENOSPC:
    case EDQUOT: return ErrorCode::NoSpace;
    default: return ErrorCode::IoError;
    }
}

Result<QByteArray> readWholeFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        const auto code =
            file.error() == QFileDevice::OpenError && !file.exists() ? ErrorCode::NotFound : ErrorCode::IoError;
        return fail(code, std::format("open '{}': {}", path.toStdString(), file.errorString().toStdString()));
    }
    return file.readAll();
}

Result<void> ensureDirectory(const QString& path, mode_t mode)
{
    const QByteArray native = QFile::encodeName(QDir::cleanPath(path));
    QByteArray partial;
    partial.reserve(native.size());
    const auto components = native.split('/');
    for (qsizetype i = 0; i < components.size(); ++i) {
        if (i > 0) {
            partial += '/';
        }
        partial += components[i];
        if (partial.isEmpty() || partial == "/") {
            continue;
        }
        if (::mkdir(partial.constData(), mode) != 0 && errno != EEXIST) {
            const int err = errno;
            return fail(errorCodeFromErrno(err), errnoDetail("mkdir", path, err));
        }
    }
    return {};
}

namespace {

enum class Publish : std::uint8_t { Replace, CreateOnly };

Result<void> writeViaTemp(const QString& path, QByteArrayView data, mode_t mode, Durability durability, Publish publish)
{
    const QFileInfo info(path);
    if (auto dir = ensureDirectory(info.absolutePath()); !dir) {
        return dir;
    }

    const QString tempPath = QStringLiteral("%1/.%2.%3.tmp")
                                 .arg(info.absolutePath(), info.fileName())
                                 .arg(QRandomGenerator::system()->generate64(), 0, 16);
    const QByteArray nativeTemp = QFile::encodeName(tempPath);
    const QByteArray nativeTarget = QFile::encodeName(info.absoluteFilePath());

    UniqueFd fd(::open(nativeTemp.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode));
    if (!fd) {
        const int err = errno;
        return fail(errorCodeFromErrno(err), errnoDetail("create", tempPath, err));
    }

    const auto cleanup = [&nativeTemp] {
        ::unlink(nativeTemp.constData());
    };

    if (auto written = writeAll(fd.get(), data, tempPath); !written) {
        cleanup();
        return written;
    }
    if (durability == Durability::Synced && ::fsync(fd.get()) != 0) {
        const int err = errno;
        cleanup();
        return fail(errorCodeFromErrno(err), errnoDetail("fsync", tempPath, err));
    }
    fd.reset();

    if (publish == Publish::Replace) {
        if (::rename(nativeTemp.constData(), nativeTarget.constData()) != 0) {
            const int err = errno;
            cleanup();
            return fail(errorCodeFromErrno(err), errnoDetail("rename", path, err));
        }
    } else {
        // link() never replaces an existing file, so of two concurrent creators exactly one wins.
        const int linked = ::link(nativeTemp.constData(), nativeTarget.constData());
        const int err = errno;
        cleanup();
        if (linked != 0) {
            return fail(err == EEXIST ? ErrorCode::AlreadyExists : errorCodeFromErrno(err),
                        errnoDetail("link", path, err));
        }
    }

    // Persist the rename itself; failure here is not fatal for correctness of the content.
    if (durability == Durability::Atomic) {
        return {};
    }
    if (const UniqueFd dirFd(
            ::open(QFile::encodeName(info.absolutePath()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        dirFd) {
        ::fsync(dirFd.get());
    }
    return {};
}

}  // namespace

Result<void> writeFileAtomic(const QString& path, QByteArrayView data, mode_t mode, Durability durability)
{
    return writeViaTemp(path, data, mode, durability, Publish::Replace);
}

Result<void> createFileAtomic(const QString& path, QByteArrayView data, mode_t mode)
{
    return writeViaTemp(path, data, mode, Durability::Synced, Publish::CreateOnly);
}

}  // namespace noctara
