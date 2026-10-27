#pragma once

#include "core/Error.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <sys/types.h>

namespace noctara {

// Reads a whole file. Missing files yield ErrorCode::NotFound.
[[nodiscard]] Result<QByteArray> readWholeFile(const QString& path);

// Writes via temp file + fsync + rename (+ directory fsync) so readers observe either the old or
// the new content, never a torn file. ENOSPC maps to ErrorCode::NoSpace.
[[nodiscard]] Result<void> writeFileAtomic(const QString& path, QByteArrayView data, mode_t mode = 0600);

// Creates the directory (and parents) with the given permissions for newly created components.
[[nodiscard]] Result<void> ensureDirectory(const QString& path, mode_t mode = 0700);

[[nodiscard]] ErrorCode errorCodeFromErrno(int err) noexcept;

}  // namespace noctara
