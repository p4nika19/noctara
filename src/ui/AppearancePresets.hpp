#pragma once

#include <QString>
#include <QVariant>

#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace noctara::ui {

// A colour preset. Presets change colours only (the scheme), never shapes, materials or layout. Applying
// one writes ordinary user settings, so everything stays visible and editable afterwards; the active preset
// is detected by comparing values.
struct AppearancePreset {
    std::string_view id;
    const char* name;  // product name of the scheme; never translated
    std::vector<std::pair<std::string_view, QVariant>> values;
    std::vector<const char*> preview;  // three colours for the preview tile: light, accent, base
};

[[nodiscard]] std::span<const AppearancePreset> appearancePresets();

}  // namespace noctara::ui
