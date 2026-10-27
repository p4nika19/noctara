#include "settings/Migrations.hpp"

#include <string>
#include <vector>

namespace noctara::settings {

namespace {

// v1 -> v2: the boolean `performance.power.battery_saver` became the three-way
// `performance.power.energy_saving` (true -> "auto", false -> "off").
void energySavingMode(toml::table& root)
{
    toml::table* power = root.at_path("performance.power").as_table();
    if (power == nullptr) {
        return;
    }
    if (const auto* old = power->get_as<bool>("battery_saver")) {
        const bool enabled = old->get();
        power->erase("battery_saver");
        power->insert_or_assign("energy_saving", enabled ? "auto" : "off");
    }
}

// v2 -> v3: the boolean `appearance.backdrop.cover_art` became `appearance.backdrop.style`
// (aurora | cover | dark). Turning the cover backdrop off meant a plain backdrop, i.e. "dark".
void backdropStyle(toml::table& root)
{
    toml::table* backdrop = root.at_path("appearance.backdrop").as_table();
    if (backdrop == nullptr) {
        return;
    }
    if (const auto* old = backdrop->get_as<bool>("cover_art")) {
        const bool enabled = old->get();
        backdrop->erase("cover_art");
        if (!enabled) {
            backdrop->insert_or_assign("style", "dark");
        }
    }
}

// Files without `config_version` are v0 (same format as v1). Append future migrations here.
constexpr Migration kMigrations[] = {
    {0, [](toml::table&) {}},
    {1, energySavingMode},
    {2, backdropStyle},
};

std::vector<std::string_view> splitPath(std::string_view path)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto dot = path.find('.', start);
        const auto end = dot == std::string_view::npos ? path.size() : dot;
        parts.push_back(path.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

}  // namespace

std::span<const Migration> builtinMigrations() noexcept
{
    return kMigrations;
}

std::int64_t migrate(toml::table& root, std::int64_t version, std::int64_t target,
                     std::span<const Migration> migrations)
{
    while (version < target) {
        const auto it = std::ranges::find(migrations, version, &Migration::from);
        if (it == migrations.end()) {
            break;
        }
        it->apply(root);
        ++version;
    }
    return version;
}

bool moveKey(toml::table& root, std::string_view from, std::string_view to)
{
    const auto fromParts = splitPath(from);
    toml::table* parent = &root;
    for (std::size_t i = 0; i + 1 < fromParts.size(); ++i) {
        parent = parent->get_as<toml::table>(fromParts[i]);
        if (parent == nullptr) {
            return false;
        }
    }
    toml::node* node = parent->get(fromParts.back());
    if (node == nullptr) {
        return false;
    }
    const auto toParts = splitPath(to);
    toml::table* target = &root;
    for (std::size_t i = 0; i + 1 < toParts.size(); ++i) {
        auto [it, inserted] = target->insert(toParts[i], toml::table{});
        (void)inserted;
        target = it->second.as_table();
        if (target == nullptr) {
            return false;
        }
    }
    node->visit([&](auto&& value) { target->insert_or_assign(toParts.back(), value); });
    parent->erase(fromParts.back());
    return true;
}

}  // namespace noctara::settings
