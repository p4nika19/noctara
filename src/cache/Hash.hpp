#pragma once

#include <QByteArray>
#include <QByteArrayView>

#include <cstdint>

namespace noctara::cache {

// Fast non-cryptographic checksum (XXH3-64) for validating stored pieces on read.
[[nodiscard]] std::uint64_t checksum(QByteArrayView data) noexcept;

// Streaming XXH3-128, e.g. for audio payload hashes across several regions of a file.
class Hasher128 {
public:
    Hasher128();
    ~Hasher128();
    Hasher128(const Hasher128&) = delete;
    Hasher128& operator=(const Hasher128&) = delete;
    Hasher128(Hasher128&&) = delete;
    Hasher128& operator=(Hasher128&&) = delete;

    void add(QByteArrayView data) noexcept;
    [[nodiscard]] QByteArray hex() const;  // 32 lower-case hex digits

private:
    void* m_state;
};

// Opaque, stable file name for a cache key: 32 hex digits of XXH3-128.
[[nodiscard]] QByteArray nameFor(QByteArrayView key);

// Content address for deduplicated blobs (covers): 64 hex digits of SHA-256.
[[nodiscard]] QByteArray contentHash(QByteArrayView data);

}  // namespace noctara::cache
