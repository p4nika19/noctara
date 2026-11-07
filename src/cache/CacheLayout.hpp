#pragma once

#include "core/Error.hpp"
#include "core/UniqueFd.hpp"

#include <QString>

namespace noctara::cache {

// <root>/audio, images, http, electron (the DRM helper's profile), plus a layout version marker and a
// CACHEDIR.TAG so backup tools skip the directory. Directories are created 0700.
struct CacheLayout {
    static constexpr int kVersion = 1;

    QString root;

    [[nodiscard]] QString audio() const { return root + QStringLiteral("/audio"); }
    [[nodiscard]] QString covers() const { return root + QStringLiteral("/audio/covers"); }
    [[nodiscard]] QString images() const { return root + QStringLiteral("/images"); }
    [[nodiscard]] QString http() const { return root + QStringLiteral("/http"); }
    [[nodiscard]] QString electron() const { return root + QStringLiteral("/electron"); }
    [[nodiscard]] QString versionFile() const { return root + QStringLiteral("/layout-version"); }
    [[nodiscard]] QString lockFile() const { return root + QStringLiteral("/lock"); }
};

// Creates the layout. If the version marker names another layout, the cache subdirectories are wiped first
// (caches are disposable; other files in the root, such as the GPU pipeline cache, are kept). Take the
// CacheLock first, so the wipe never runs under another instance using the caches.
// Fails with PermissionDenied if the root is not a writable directory.
[[nodiscard]] Result<CacheLayout> prepareLayout(const QString& root);

// Advisory lock (flock) on <root>/lock, so a second Noctara instance does not write to the same caches.
class CacheLock {
public:
    CacheLock() = default;
    // Tries to take the lock without waiting.
    [[nodiscard]] static Result<CacheLock> acquire(const CacheLayout& layout);

private:
    explicit CacheLock(UniqueFd fd)
        : m_fd(std::move(fd))
    {
    }
    UniqueFd m_fd;
};

}  // namespace noctara::cache
