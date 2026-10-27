#pragma once

#include "core/Error.hpp"

#include <QString>
#include <QStringList>
#include <QVariant>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace noctara::settings {

enum class Type : std::uint8_t {
    Bool,
    Int,
    Real,
    String,
    Enum,
    Color,  // "#rrggbb" or "#aarrggbb", normalised to lower case
    Path,   // empty = platform default
    Url,
    StringList,
    Shortcut,  // QKeySequence portable text, e.g. "Ctrl+K"
};

// Settings pages, in display order.
enum class Category : std::uint8_t {
    General,
    Appearance,
    Layout,
    Playback,
    Library,
    Keyboard,
    Performance,
    Network,
    Storage,
    Accounts,
    Advanced,
    About,
};
inline constexpr int kCategoryCount = static_cast<int>(Category::About) + 1;

[[nodiscard]] std::string_view categoryId(Category category) noexcept;
[[nodiscard]] const char* categoryLabel(Category category) noexcept;  // translation source, context "Settings"

namespace flag {
inline constexpr std::uint8_t RequiresRestart = 1U << 0U;
inline constexpr std::uint8_t Advanced = 1U << 1U;
inline constexpr std::uint8_t ConfigOnly = 1U << 2U;  // no UI row (e.g. data edited in dedicated editors)
}  // namespace flag

struct Option {
    std::string_view id;       // stable config value
    const char* label;         // UTF-8; translation source in context "Settings" unless !translatable
    bool translatable = true;  // false for e.g. language endonyms ("Русский")
};

// Row is shown only while `key` currently holds one of `values` (UI dependency, not a validation rule).
struct Condition {
    std::string_view key;
    std::vector<std::string_view> values;
};

using Validator = Result<void> (*)(const QVariant& normalised);

// One setting. Translation sources (label/description/unit/section) use the "Settings" context.
struct Definition {
    std::string_view key;  // "network.dns.mode"; the part before the last dot is the section / TOML table
    Type type = Type::Bool;
    QVariant defaultValue;
    double min = 0.0;  // numeric bounds; min == max means unbounded
    double max = 0.0;
    double step = 0.0;
    std::vector<Option> options;
    Category category = Category::General;
    const char* label = nullptr;
    const char* description = nullptr;
    const char* unit = nullptr;
    std::uint8_t flags = 0;
    std::optional<Condition> visibleWhen;
    Validator validator = nullptr;
    std::string_view cliAlias;  // optional `--<alias>=<value>` shortcut

    [[nodiscard]] bool requiresRestart() const noexcept { return (flags & flag::RequiresRestart) != 0; }
    [[nodiscard]] bool advanced() const noexcept { return (flags & flag::Advanced) != 0; }
    [[nodiscard]] bool configOnly() const noexcept { return (flags & flag::ConfigOnly) != 0; }
    [[nodiscard]] bool bounded() const noexcept { return min != max; }
    [[nodiscard]] std::string_view sectionId() const noexcept;
    [[nodiscard]] std::string_view leafName() const noexcept;
};

struct Section {
    std::string_view id;  // equals the TOML table path, e.g. "network.dns"
    Category category = Category::General;
    const char* label = nullptr;
    const char* description = nullptr;
};

class Schema {
public:
    static constexpr std::int64_t kConfigVersion = 2;

    // Validates the schema itself (unique keys, sections declared, defaults valid); aborts on
    // programmer error since a broken schema is a build defect, not a runtime condition.
    Schema(std::vector<Section> sections, std::vector<Definition> definitions);

    [[nodiscard]] static const Schema& builtin();

    [[nodiscard]] std::span<const Definition> definitions() const noexcept { return m_definitions; }
    [[nodiscard]] std::span<const Section> sections() const noexcept { return m_sections; }
    [[nodiscard]] std::size_t size() const noexcept { return m_definitions.size(); }

    [[nodiscard]] std::optional<std::size_t> indexOf(std::string_view key) const noexcept;
    [[nodiscard]] const Definition* find(std::string_view key) const noexcept;
    [[nodiscard]] const Definition& at(std::size_t index) const { return m_definitions.at(index); }
    [[nodiscard]] const Section* section(std::string_view id) const noexcept;

    [[nodiscard]] static QString environmentVariable(const Definition& def);

private:
    std::vector<Section> m_sections;
    std::vector<Definition> m_definitions;
    std::unordered_map<std::string_view, std::size_t> m_index;
};

// Checks type, range, enum membership and custom validators. Returns the normalised value
// (e.g. integral reals become qint64 for Int settings, colours are lower-cased).
[[nodiscard]] Result<QVariant> validate(const Definition& def, const QVariant& candidate);

// Parses a textual value (environment variable, CLI flag) for the given setting, then validates it.
[[nodiscard]] Result<QVariant> parseText(const Definition& def, QStringView text);

// TOML literal for a validated value: true, 42, 0.5, "text", ["a", "b"].
[[nodiscard]] QString formatTomlValue(const Definition& def, const QVariant& value);

[[nodiscard]] std::string_view typeName(Type type) noexcept;

// Index-addressed values for one configuration layer.
using ValueMap = std::vector<std::pair<std::size_t, QVariant>>;

}  // namespace noctara::settings
