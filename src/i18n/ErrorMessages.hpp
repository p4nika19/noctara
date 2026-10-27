#pragma once

#include "core/Error.hpp"
#include "settings/ConfigIssue.hpp"

#include <QString>

namespace noctara::i18n {

// Translated, actionable message for a typed error code (context "Errors").
[[nodiscard]] QString errorMessage(ErrorCode code);

// Translated description of a configuration problem, including where it is.
[[nodiscard]] QString configIssueMessage(const settings::ConfigIssue& issue);

}  // namespace noctara::i18n
