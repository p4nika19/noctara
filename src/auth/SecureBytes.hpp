#pragma once

#include <QByteArray>

#include <cstddef>
#include <openssl/crypto.h>

namespace noctara::auth {

// Overwrites a secret before its buffer is released. A buffer still shared with another QByteArray is only
// released: writing to it would detach, so the wipe would clear a fresh copy and leave the shared original intact.
inline void wipe(QByteArray& bytes) noexcept
{
    if (!bytes.isEmpty() && bytes.isDetached()) {
        OPENSSL_cleanse(bytes.data(), static_cast<std::size_t>(bytes.size()));
    }
    bytes.clear();
}

// Wipes the referenced buffer when the scope ends, on every return path.
class WipeOnExit {
public:
    explicit WipeOnExit(QByteArray& bytes) noexcept
        : m_bytes(bytes)
    {
    }
    ~WipeOnExit() { wipe(m_bytes); }
    WipeOnExit(const WipeOnExit&) = delete;
    WipeOnExit& operator=(const WipeOnExit&) = delete;
    WipeOnExit(WipeOnExit&&) = delete;
    WipeOnExit& operator=(WipeOnExit&&) = delete;

private:
    QByteArray& m_bytes;
};

}  // namespace noctara::auth
