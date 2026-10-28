#pragma once

#include "core/Error.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <cstdint>
#include <sys/types.h>

namespace noctara {

// Reads a whole file. Missing files yield ErrorCode::NotFound.
[[nodiscard]] Result<QByteArray> readWholeFile(const QString& path);

enum class Durability : std::uint8_t {
    Synced,  // fsync the file and its directory: the new content survives a power loss
    Atomic,  // only atomic: after a crash the file holds the old or the new content (caches re-validate anyway)
};

// Writes via temp file (+ fsync) + rename (+ directory fsync) so readers observe either the old or
// the new content, never a torn file. ENOSPC maps to ErrorCode::NoSpace.
[[nodiscard]] Result<void> writeFileAtomic(const QString& path, QByteArrayView data, mode_t mode = 0600,
                                           Durability durability = Durability::Synced);

// Like writeFileAtomic, but never replaces an existing file: fails with ErrorCode::AlreadyExists instead, so
// concurrent creators (two processes) agree on one content.
[[nodiscard]] Result<void> createFileAtomic(const QString& path, QByteArrayView data, mode_t mode = 0600);

// Creates the directory (and parents) with the given permissions for newly created components.
[[nodiscard]] Result<void> ensureDirectory(const QString& path, mode_t mode = 0700);

[[nodiscard]] ErrorCode errorCodeFromErrno(int err) noexcept;

}  // namespace noctara
