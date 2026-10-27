#pragma once

// Single include point for toml++ so every translation unit sees the same configuration (ODR).
// Header-only: the distro's compiled libtomlplusplus is built with exceptions, we build without them.
#undef TOML_HEADER_ONLY
#undef TOML_SHARED_LIB
#define TOML_HEADER_ONLY 1        // NOLINT(cppcoreguidelines-macro-usage)
#define TOML_EXCEPTIONS 0         // NOLINT(cppcoreguidelines-macro-usage)
#define TOML_ENABLE_FORMATTERS 1  // NOLINT(cppcoreguidelines-macro-usage)
#include <toml++/toml.hpp>
