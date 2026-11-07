#pragma once

#include "settings/ConfigIssue.hpp"
#include "settings/Schema.hpp"

#include <QList>
#include <QProcessEnvironment>
#include <QString>

#include <cstdint>

namespace noctara::settings {

struct OverrideSet {
    ValueMap values;
    ConfigIssues issues;
};

// Reads NOCTARA_<KEY> variables (dots become underscores, upper case).
[[nodiscard]] OverrideSet collectEnvironment(const Schema& schema, const QProcessEnvironment& environment);

// One settings flag from the command line: a "key=value" string from `--set`, or an "alias=value" string for
// a per-setting shortcut declared in the schema (e.g. --gpu=off).
struct CommandLineOverride {
    enum class Kind : std::uint8_t { Set, Alias };
    Kind kind = Kind::Set;
    QString text;
};

// Applies the flags in the order given (their order on the command line): the later of two flags for one key wins.
[[nodiscard]] OverrideSet collectCommandLine(const Schema& schema, const QList<CommandLineOverride>& overrides);

}  // namespace noctara::settings
