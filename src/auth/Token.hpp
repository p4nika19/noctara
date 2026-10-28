#pragma once

#include <QByteArrayView>

#include <algorithm>

namespace noctara::auth {

// Whether `token` can be a SoundCloud OAuth token: 1–4096 visible ASCII characters. Anything else (spaces, CR/LF,
// non-ASCII) is refused before it could reach an Authorization header.
[[nodiscard]] inline bool isPlausibleToken(QByteArrayView token) noexcept
{
    constexpr qsizetype maxSize = 4096;
    return !token.isEmpty() && token.size() <= maxSize && std::ranges::all_of(token, [](const char value) {
        const auto byte = static_cast<unsigned char>(value);
        return byte >= 0x21 && byte <= 0x7e;
    });
}

}  // namespace noctara::auth
