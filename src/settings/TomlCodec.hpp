#pragma once

#include "settings/ConfigIssue.hpp"
#include "settings/Schema.hpp"

#include <QByteArray>

#include <string>
#include <string_view>

namespace noctara::settings {

struct ParsedConfig {
    ValueMap values;            // validated values for known keys
    ConfigIssues issues;        // syntax errors, invalid values, unknown keys (warnings)
    std::string unknownToml;    // keys outside the schema, re-emitted verbatim on save
    std::int64_t version = 0;   // config_version found in the file (0 = absent)
    bool syntaxOk = true;
};

// Parses a config file. Never throws; a syntax error yields syntaxOk = false and no values.
[[nodiscard]] ParsedConfig parseConfig(std::string_view text, const QString& fileName, const Schema& schema);

// Produces a fully documented config file: every schema key appears with its description, keys at their
// default are commented out, user values are written, unknown keys are preserved.
[[nodiscard]] QByteArray serializeConfig(const Schema& schema, const ValueMap& userValues,
                                         std::string_view unknownToml,
                                         std::int64_t version = Schema::kConfigVersion);

}  // namespace noctara::settings
