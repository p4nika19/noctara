#include "ui/AppearancePresets.hpp"

#include "settings/Keys.hpp"

namespace noctara::ui {

namespace {

namespace keys = settings::keys;

QVariant text(const char* value)
{
    return QVariant::fromValue(QString::fromLatin1(value));
}

std::vector<AppearancePreset> build()
{
    return {
        // Follows the desktop colour scheme: Midnight while it is dark, Frost while it is light.
        {"auto",
         QT_TRANSLATE_NOOP("Presets", "Auto"),
         {{keys::ThemeMode, text("system")}},
         {"#06070f", "#6774f9", "#f4f6fd"},
         true},
        {"midnight", "Midnight", {{keys::ThemeMode, text("dark")}}, {"#8f9bff", "#6774f9", "#06070f"}},
        {"frost", "Frost", {{keys::ThemeMode, text("light")}}, {"#ffffff", "#7a8cff", "#dfe6f5"}},
    };
}

}  // namespace

std::span<const AppearancePreset> appearancePresets()
{
    static const std::vector<AppearancePreset> presets = build();
    return presets;
}

}  // namespace noctara::ui
