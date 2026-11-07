#pragma once

#include "settings/ConfigIssue.hpp"
#include "settings/TomlInclude.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace noctara::settings {

// Upgrades a parsed config table from `from` to `from + 1` (renames, value remapping).
struct Migration {
    std::int64_t from;
    void (*apply)(toml::table& root);
};

[[nodiscard]] std::span<const Migration> builtinMigrations() noexcept;

// Applies migrations in order until `target` is reached. Returns the resulting version.
std::int64_t migrate(toml::table& root, std::int64_t version, std::int64_t target,
                     std::span<const Migration> migrations);

// "a.b.c" -> {"a", "b", "c"} (an empty path gives one empty part).
[[nodiscard]] std::vector<std::string_view> splitPath(std::string_view path);

// Helper for migrations: moves a value between dotted paths, creating tables as needed.
bool moveKey(toml::table& root, std::string_view from, std::string_view to);

}  // namespace noctara::settings
