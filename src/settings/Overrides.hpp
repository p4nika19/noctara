#pragma once

#include "settings/ConfigIssue.hpp"
#include "settings/Schema.hpp"

#include <QProcessEnvironment>
#include <QStringList>

namespace noctara::settings {

struct OverrideSet {
    ValueMap values;
    ConfigIssues issues;
};

// Reads NOCTARA_<KEY> variables (dots become underscores, upper case).
[[nodiscard]] OverrideSet collectEnvironment(const Schema& schema, const QProcessEnvironment& environment);

// `assignments` are "key=value" strings from repeated `--set` options; `aliases` are "alias=value"
// strings for the per-setting shortcuts declared in the schema (e.g. --gpu=off).
[[nodiscard]] OverrideSet collectCommandLine(const Schema& schema, const QStringList& assignments,
                                             const QStringList& aliases);

}  // namespace noctara::settings
