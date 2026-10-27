#pragma once

#include <string_view>

// Stable configuration keys. They are part of the config file format: never rename without a migration.
namespace noctara::settings::keys {

inline constexpr std::string_view Language = "general.language";
inline constexpr std::string_view TimeFormat = "general.formatting.time_format";
inline constexpr std::string_view CompactNumbers = "general.formatting.compact_numbers";

inline constexpr std::string_view ThemeMode = "appearance.theme.mode";
inline constexpr std::string_view EnforceContrast = "appearance.theme.enforce_contrast";
inline constexpr std::string_view AccentAdaptive = "appearance.accent.adaptive";
inline constexpr std::string_view AccentColor = "appearance.accent.color";
inline constexpr std::string_view AccentExtraction = "appearance.accent.extraction";
inline constexpr std::string_view BackdropStyle = "appearance.backdrop.style";
inline constexpr std::string_view GlowIntensity = "appearance.glow.intensity";
inline constexpr std::string_view GlowCover = "appearance.glow.cover";
inline constexpr std::string_view GlowControls = "appearance.glow.controls";
inline constexpr std::string_view GlowProgress = "appearance.glow.progress";
inline constexpr std::string_view ButtonMaterial = "appearance.glass.buttons";
inline constexpr std::string_view ButtonShape = "appearance.shape.buttons";
inline constexpr std::string_view ColorTransition = "appearance.motion.color_transition";
inline constexpr std::string_view Density = "layout.content.density";
inline constexpr std::string_view BackdropAnimate = "appearance.backdrop.animate";
inline constexpr std::string_view GlassStyle = "appearance.glass.style";
inline constexpr std::string_view GlassBlurStrength = "appearance.glass.blur_strength";
inline constexpr std::string_view GlassOpacity = "appearance.glass.opacity";
inline constexpr std::string_view GlassGrain = "appearance.glass.grain";
// "Glass tint" and "Grain" are stored relative to the designed values: 0 is the design, the scale is the same as
// the absolute 0..1 one it replaced (config v4). Effective value = setting + baseline.
inline constexpr double GlassOpacityBaseline = 0.5;
inline constexpr double GlassGrainBaseline = 0.35;
inline constexpr std::string_view GlassQuality = "appearance.glass.quality";
inline constexpr std::string_view ReduceTransparency = "appearance.glass.reduce_transparency";
inline constexpr std::string_view ReduceMotion = "appearance.motion.reduce";
inline constexpr std::string_view AnimationSpeed = "appearance.motion.speed";
inline constexpr std::string_view CornerRadius = "appearance.shape.corner_radius";
inline constexpr std::string_view CornerRadiusMultiplier = "appearance.shape.corner_radius_multiplier";
inline constexpr std::string_view CoverArtShape = "appearance.shape.cover_art";
inline constexpr std::string_view UiScale = "appearance.text.ui_scale";
inline constexpr std::string_view FontScale = "appearance.text.font_scale";
inline constexpr std::string_view HeadingFont = "appearance.text.heading_font";
inline constexpr std::string_view BodyFont = "appearance.text.body_font";
inline constexpr std::string_view MonoFont = "appearance.text.mono_font";
inline constexpr std::string_view IconWeight = "appearance.icons.weight";

inline constexpr std::string_view GpuAcceleration = "performance.gpu.acceleration";
inline constexpr std::string_view GraphicsApi = "performance.gpu.api";
inline constexpr std::string_view GpuPreference = "performance.gpu.preference";
inline constexpr std::string_view VSync = "performance.rendering.vsync";
inline constexpr std::string_view FrameRateCap = "performance.rendering.frame_rate_cap";
inline constexpr std::string_view EnergySaving = "performance.power.energy_saving";

}  // namespace noctara::settings::keys
