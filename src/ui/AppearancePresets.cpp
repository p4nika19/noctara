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
        {"midnight",
         QT_TRANSLATE_NOOP("Presets", "Midnight"),
         {{keys::AccentExtraction, text("vivid")},
          {keys::BackdropStyle, text("aurora")},
          {keys::ButtonMaterial, text("frosted")},
          {keys::GlassBlurStrength, 0.6},
          {keys::GlassOpacity, 0.5},
          {keys::GlassGrain, 0.35},
          {keys::GlowIntensity, 0.3},
          {keys::GlowCover, true},
          {keys::GlowControls, true},
          {keys::GlowProgress, true},
          {keys::CornerRadius, text("default")},
          {keys::ButtonShape, text("pill")},
          {keys::Density, text("cozy")},
          {keys::ColorTransition, text("smooth")}},
         {"#8f9bff", "#6774f9", "#06070f"}},
        {"glacier",
         QT_TRANSLATE_NOOP("Presets", "Glacier"),
         {{keys::AccentExtraction, text("soft")},
          {keys::BackdropStyle, text("cover")},
          {keys::ButtonMaterial, text("liquid")},
          {keys::GlassBlurStrength, 0.9},
          {keys::GlassOpacity, 0.65},
          {keys::GlassGrain, 0.5},
          {keys::GlowIntensity, 0.15},
          {keys::GlowCover, true},
          {keys::GlowControls, false},
          {keys::GlowProgress, false},
          {keys::CornerRadius, text("round")},
          {keys::ButtonShape, text("pill")},
          {keys::Density, text("airy")},
          {keys::ColorTransition, text("slow")}},
         {"#dfeaff", "#7a8cff", "#101a33"}},
        {"neon",
         QT_TRANSLATE_NOOP("Presets", "Neon"),
         {{keys::AccentExtraction, text("vivid")},
          {keys::BackdropStyle, text("aurora")},
          {keys::ButtonMaterial, text("tinted")},
          {keys::GlassBlurStrength, 0.3},
          {keys::GlassOpacity, 0.35},
          {keys::GlassGrain, 0.2},
          {keys::GlowIntensity, 0.85},
          {keys::GlowCover, true},
          {keys::GlowControls, true},
          {keys::GlowProgress, true},
          {keys::CornerRadius, text("sharp")},
          {keys::ButtonShape, text("soft")},
          {keys::Density, text("compact")},
          {keys::ColorTransition, text("fast")}},
         {"#ff5fa2", "#8b6cff", "#0a0414"}},
    };
}

}  // namespace

std::span<const AppearancePreset> appearancePresets()
{
    static const std::vector<AppearancePreset> presets = build();
    return presets;
}

}  // namespace noctara::ui
