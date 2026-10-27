#pragma once

#include "settings/Schema.hpp"
#include "settings/SettingsStore.hpp"

#include <QString>

namespace noctara::settings {

// docs/configuration.md, generated from the schema (English source strings).
[[nodiscard]] QString schemaMarkdown(const Schema& schema);

// `--print-config`: effective value of every key with its source layer. URL credentials are redacted;
// the schema itself never holds secrets.
[[nodiscard]] QString effectiveConfigReport(const SettingsStore& store);

}  // namespace noctara::settings
