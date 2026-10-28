#include "cache/Hash.hpp"

#include <QCryptographicHash>

#include <xxhash.h>

namespace noctara::cache {

std::uint64_t checksum(QByteArrayView data) noexcept
{
    return XXH3_64bits(data.data(), static_cast<std::size_t>(data.size()));
}

Hasher128::Hasher128()
    : m_state(XXH3_createState())
{
    XXH3_128bits_reset(static_cast<XXH3_state_t*>(m_state));
}

Hasher128::~Hasher128()
{
    XXH3_freeState(static_cast<XXH3_state_t*>(m_state));
}

void Hasher128::add(QByteArrayView data) noexcept
{
    XXH3_128bits_update(static_cast<XXH3_state_t*>(m_state), data.data(), static_cast<std::size_t>(data.size()));
}

QByteArray Hasher128::hex() const
{
    const XXH128_hash_t hash = XXH3_128bits_digest(static_cast<XXH3_state_t*>(m_state));
    XXH128_canonical_t canonical;
    XXH128_canonicalFromHash(&canonical, hash);
    return QByteArray(reinterpret_cast<const char*>(canonical.digest), sizeof(canonical.digest)).toHex();
}

QByteArray nameFor(QByteArrayView key)
{
    Hasher128 hasher;
    hasher.add(key);
    return hasher.hex();
}

QByteArray contentHash(QByteArrayView data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

}  // namespace noctara::cache
