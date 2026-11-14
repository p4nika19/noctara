#include "settings/Migrations.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
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

// Config v4 briefly stored "Glass tint" and "Grain" relative to these values; v5 is absolute (0..1) again.
constexpr std::pair<const char*, double> kV4GlassBaselines[] = {{"opacity", 0.5}, {"grain", 0.35}};

void shiftGlassValues(toml::table& root, double direction)
{
    toml::table* glass = root.at_path("appearance.glass").as_table();
    if (glass == nullptr) {
        return;
    }
    for (const auto& [key, baseline] : kV4GlassBaselines) {
        if (const auto value = glass->get(key) ? glass->get(key)->value<double>() : std::nullopt) {
            glass->insert_or_assign(key, *value + direction * baseline);
        }
    }
}

// v3 -> v4: absolute "Glass tint" and "Grain" became relative to the designed values.
void relativeGlassValues(toml::table& root)
{
    shiftGlassValues(root, -1.0);
}

// v4 -> v5: back to absolute values (0 = no grain, lightest tint). A v3 file passes both steps unchanged.
void absoluteGlassValues(toml::table& root)
{
    shiftGlassValues(root, 1.0);
}

// v5 -> v6: the "Glass tint" scale moved by 20 % (the former 0 % is now 20 %, the former 80 % now 100 %),
// so a stored tint keeps its look.
void shiftedGlassTint(toml::table& root)
{
    toml::table* glass = root.at_path("appearance.glass").as_table();
    if (glass == nullptr) {
        return;
    }
    if (const auto value = glass->get("opacity") ? glass->get("opacity")->value<double>() : std::nullopt) {
        glass->insert_or_assign("opacity", std::min(1.0, *value + 0.2));
    }
}

// v6 -> v7: "Glass style" mixed the blur with the look of surfaces. It became the blur only
// (`appearance.glass.style`: kawase | gaussian | solid) plus `appearance.glass.surface` (frosted | liquid | flat);
// "liquid" was Liquid Glass surfaces over the Dual Kawase blur.
void splitGlassStyle(toml::table& root)
{
    toml::table* glass = root.at_path("appearance.glass").as_table();
    if (glass == nullptr) {
        return;
    }
    if (const auto* style = glass->get_as<std::string>("style"); style != nullptr && style->get() == "liquid") {
        glass->insert_or_assign("style", "kawase");
        glass->insert_or_assign("surface", "liquid");
    }
}

// v7 -> v8: the Downloaded page joined the navigation. A list of pages someone chose gets it too, right after
// Charts (or at the end without Charts); a file without a list gets the new default anyway.
void downloadsPage(toml::table& root)
{
    toml::array* items = root.at_path("layout.navigation.items").as_array();
    if (items == nullptr) {
        return;
    }
    std::size_t at = items->size();
    for (std::size_t i = 0; i < items->size(); ++i) {
        const std::optional<std::string_view> id = (*items)[i].value<std::string_view>();
        if (id == "downloads") {
            return;
        }
        if (id == "charts") {
            at = i + 1;
        }
    }
    items->insert(items->cbegin() + static_cast<std::ptrdiff_t>(at), "downloads");
}

// Files without `config_version` are v0 (same format as v1). Append future migrations here.
// clang-format off
constexpr Migration kMigrations[] = {
    {0, [](toml::table&) {}},
    {1, energySavingMode},
    {2, backdropStyle},
    {3, relativeGlassValues},
    {4, absoluteGlassValues},
    {5, shiftedGlassTint},
    {6, splitGlassStyle},
    {7, downloadsPage},
};
// clang-format on

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
