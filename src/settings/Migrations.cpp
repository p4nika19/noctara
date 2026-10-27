#include "settings/Migrations.hpp"

#include <string>
#include <vector>

namespace noctara::settings {

namespace {

// Schema version 1 is the first released format; files without `config_version` are treated as v0
// and need no transformation. Future renames are appended here.
constexpr Migration kMigrations[] = {
    {0, [](toml::table&) {}},
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
