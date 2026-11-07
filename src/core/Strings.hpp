#pragma once

#include <QString>

#include <string_view>

namespace noctara {

// Schema keys, ids and other ASCII text kept as std::string_view, as a QString.
[[nodiscard]] inline QString latin(std::string_view text)
{
    return QString::fromLatin1(text.data(), static_cast<qsizetype>(text.size()));
}

}  // namespace noctara
