#pragma once

#include "core/Error.hpp"

#include <QList>
#include <QString>

namespace noctara::settings {

// A problem found while reading configuration. `detail` is English (logged); the UI builds a translated
// message from `code`, `key`, `file` and `line`.
struct ConfigIssue {
    enum class Severity : std::uint8_t { Warning, Error };

    Severity severity = Severity::Error;
    ErrorCode code = ErrorCode::ConfigInvalidValue;
    QString file;
    int line = 0;
    int column = 0;
    QString key;
    QString detail;

    [[nodiscard]] QString logText() const;
};

using ConfigIssues = QList<ConfigIssue>;

}  // namespace noctara::settings
