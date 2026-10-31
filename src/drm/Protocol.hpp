#pragma once

#include "core/Error.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QJsonObject>
#include <QString>

#include <cstdint>

namespace noctara::drm {

// Noctara ↔ protected-player helper messages: one JSON object per line, each with a "type". The protocol is
// defined once, in electron/protocol.json: the helper reads that file and this module embeds it, so both sides
// validate against the same definition (see docs/ipc.md).
enum class Direction : std::uint8_t {
    ToHelper,    // Noctara → helper (stdin)
    FromHelper,  // helper → Noctara (stdout)
};

[[nodiscard]] int protocolVersion();

// Empty when `message` is a valid message for `direction`, otherwise what is wrong with it (English, for logs).
[[nodiscard]] QString validate(Direction direction, const QJsonObject& message);

// One received line: parsed and validated.
[[nodiscard]] Result<QJsonObject> decode(QByteArrayView line, Direction direction);
// One line to send (compact JSON and a newline). The caller validates first.
[[nodiscard]] QByteArray encode(const QJsonObject& message);

}  // namespace noctara::drm
