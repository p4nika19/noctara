// The application's settings schema: the single source of truth for the settings UI, config file,
// CLI/env overrides and docs/configuration.md. Adding a setting = adding one entry here.
#include "settings/Keys.hpp"
#include "settings/Schema.hpp"

// Entries rely on designated initializers leaving defaulted fields out; that is the intended idiom here.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"

namespace noctara::settings {

namespace {

using enum Category;

QVariant str(const char* text)
{
    return QVariant::fromValue(QString::fromUtf8(text));
}

QVariant integer(qint64 value)
{
    return QVariant::fromValue(value);
}

QVariant real(double value)
{
    return QVariant::fromValue(value);
}

std::vector<Section> builtinSections()
{
    return {
        {"general", General, QT_TRANSLATE_NOOP("Settings", "Language"), QT_TRANSLATE_NOOP("Settings", "Interface language.")},
        {"general.formatting", General, QT_TRANSLATE_NOOP("Settings", "Formatting"),
         QT_TRANSLATE_NOOP("Settings", "How times and numbers are displayed.")},

        {"appearance.theme", Appearance, QT_TRANSLATE_NOOP("Settings", "Theme"),
         QT_TRANSLATE_NOOP("Settings", "Midnight (dark), Frost (light) or follow the system.")},
        {"appearance.accent", Appearance, QT_TRANSLATE_NOOP("Settings", "Accent color"),
         QT_TRANSLATE_NOOP("Settings", "The whole palette is generated from a single accent color.")},
        {"appearance.backdrop", Appearance, QT_TRANSLATE_NOOP("Settings", "Backdrop"),
         QT_TRANSLATE_NOOP("Settings", "The blurred cover art behind the interface.")},
        {"appearance.glass", Appearance, QT_TRANSLATE_NOOP("Settings", "Glass"),
         QT_TRANSLATE_NOOP("Settings", "How translucent surfaces are rendered.")},
        {"appearance.motion", Appearance, QT_TRANSLATE_NOOP("Settings", "Motion"),
         QT_TRANSLATE_NOOP("Settings", "Animations and transitions.")},
        {"appearance.shape", Appearance, QT_TRANSLATE_NOOP("Settings", "Shape"),
         QT_TRANSLATE_NOOP("Settings", "Corner rounding of surfaces and artwork.")},
        {"appearance.text", Appearance, QT_TRANSLATE_NOOP("Settings", "Text and scale"),
         QT_TRANSLATE_NOOP("Settings", "Interface zoom and fonts. Missing fonts fall back to the bundled ones.")},
        {"appearance.icons", Appearance, QT_TRANSLATE_NOOP("Settings", "Icons"),
         QT_TRANSLATE_NOOP("Settings", "Icon appearance.")},

        {"performance.gpu", Performance, QT_TRANSLATE_NOOP("Settings", "Graphics"),
         QT_TRANSLATE_NOOP("Settings", "GPU acceleration. Changes take effect after a restart.")},
        {"performance.rendering", Performance, QT_TRANSLATE_NOOP("Settings", "Rendering"),
         QT_TRANSLATE_NOOP("Settings", "Frame pacing.")},
        {"performance.power", Performance, QT_TRANSLATE_NOOP("Settings", "Power"),
         QT_TRANSLATE_NOOP("Settings", "Energy saving.")},
    };
}

std::vector<Definition> builtinDefinitions()
{
    std::vector<Definition> defs;

    // --- General -------------------------------------------------------------------------------
    defs.push_back({.key = keys::Language,
                    .type = Type::Enum,
                    .defaultValue = str("system"),
                    .options = {{"system", QT_TRANSLATE_NOOP("Settings", "System default")},
                                {"en", "English", false},
                                {"ru", "Русский", false}},
                    .category = General,
                    .label = QT_TRANSLATE_NOOP("Settings", "Language"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Applied immediately, without a restart."),
                    .cliAlias = "lang"});
    defs.push_back({.key = keys::TimeFormat,
                    .type = Type::Enum,
                    .defaultValue = str("auto"),
                    .options = {{"auto", QT_TRANSLATE_NOOP("Settings", "Follow language")},
                                {"12h", QT_TRANSLATE_NOOP("Settings", "12-hour")},
                                {"24h", QT_TRANSLATE_NOOP("Settings", "24-hour")}},
                    .category = General,
                    .label = QT_TRANSLATE_NOOP("Settings", "Time format"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Clock style for dates and times.")});
    defs.push_back({.key = keys::CompactNumbers,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = General,
                    .label = QT_TRANSLATE_NOOP("Settings", "Compact numbers"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Show large counts as 1.2K instead of 1,234.")});

    // --- Appearance: theme -----------------------------------------------------------------------
    defs.push_back({.key = keys::ThemeMode,
                    .type = Type::Enum,
                    .defaultValue = str("system"),
                    .options = {{"system", QT_TRANSLATE_NOOP("Settings", "System")},
                                {"dark", QT_TRANSLATE_NOOP("Settings", "Midnight (dark)")},
                                {"light", QT_TRANSLATE_NOOP("Settings", "Frost (light)")}},
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Theme"),
                    .description = QT_TRANSLATE_NOOP("Settings", "System follows the desktop color scheme live."),
                    .cliAlias = "theme"});
    defs.push_back({.key = keys::EnforceContrast,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Enforce readable contrast"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Adjust colors that would fall below WCAG AA contrast against glass.")});

    // --- Appearance: accent ----------------------------------------------------------------------
    defs.push_back({.key = keys::AccentAdaptive,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Adapt accent colors to cover art"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Derive the accent palette from the current track's artwork.")});
    defs.push_back({.key = keys::AccentColor,
                    .type = Type::Color,
                    .defaultValue = str("#7c6cf0"),
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Accent color"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Seed color for the whole palette when adaptive accents are off."),
                    .visibleWhen = Condition{keys::AccentAdaptive, {"false"}}});
    defs.push_back({.key = keys::BackdropCoverArt,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Use cover art as backdrop"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Show a blurred version of the current artwork behind the interface.")});
    defs.push_back({.key = keys::BackdropAnimate,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Animate backdrop"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Slow drifting motion. Paused automatically when the window is hidden.")});

    // --- Appearance: glass -----------------------------------------------------------------------
    defs.push_back({.key = keys::GlassStyle,
                    .type = Type::Enum,
                    .defaultValue = str("kawase"),
                    .options = {{"kawase", QT_TRANSLATE_NOOP("Settings", "Dual Kawase blur")},
                                {"gaussian", QT_TRANSLATE_NOOP("Settings", "Gaussian blur")},
                                {"liquid", QT_TRANSLATE_NOOP("Settings", "Liquid Glass")},
                                {"solid", QT_TRANSLATE_NOOP("Settings", "Solid (no blur)")}},
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Glass style"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Blur styles need hardware acceleration; Solid is the cheapest.")});
    defs.push_back({.key = keys::GlassBlurStrength,
                    .type = Type::Real,
                    .defaultValue = real(0.6),
                    .min = 0.0,
                    .max = 1.0,
                    .step = 0.05,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Blur strength"),
                    .description = QT_TRANSLATE_NOOP("Settings", "How strongly the backdrop is blurred."),
                    .visibleWhen = Condition{keys::GlassStyle, {"kawase", "gaussian", "liquid"}}});
    defs.push_back({.key = keys::GlassOpacity,
                    .type = Type::Real,
                    .defaultValue = real(0.5),
                    .min = 0.0,
                    .max = 1.0,
                    .step = 0.05,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Glass tint"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Opacity of the tint applied to glass surfaces.")});
    defs.push_back({.key = keys::GlassGrain,
                    .type = Type::Real,
                    .defaultValue = real(0.35),
                    .min = 0.0,
                    .max = 1.0,
                    .step = 0.05,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Grain"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Fine noise that prevents banding in gradients.")});
    defs.push_back({.key = keys::GlassQuality,
                    .type = Type::Enum,
                    .defaultValue = str("auto"),
                    .options = {{"auto", QT_TRANSLATE_NOOP("Settings", "Auto")},
                                {"low", QT_TRANSLATE_NOOP("Settings", "Low")},
                                {"medium", QT_TRANSLATE_NOOP("Settings", "Medium")},
                                {"high", QT_TRANSLATE_NOOP("Settings", "High")}},
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Effects quality"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Auto lowers quality when frames take too long and raises it again later.")});
    defs.push_back({.key = keys::ReduceTransparency,
                    .type = Type::Bool,
                    .defaultValue = false,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Reduce transparency"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Use opaque surfaces everywhere.")});

    // --- Appearance: motion ----------------------------------------------------------------------
    defs.push_back({.key = keys::ReduceMotion,
                    .type = Type::Bool,
                    .defaultValue = false,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Reduce motion"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Replace movement with simple fades and stop the drifting backdrop.")});
    defs.push_back({.key = keys::AnimationSpeed,
                    .type = Type::Real,
                    .defaultValue = real(1.0),
                    .min = 0.0,
                    .max = 3.0,
                    .step = 0.25,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Animation speed"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Duration multiplier. 0 turns animations off."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "×")});

    // --- Appearance: shape -----------------------------------------------------------------------
    defs.push_back({.key = keys::CornerRadius,
                    .type = Type::Enum,
                    .defaultValue = str("default"),
                    .options = {{"sharp", QT_TRANSLATE_NOOP("Settings", "Sharp")},
                                {"default", QT_TRANSLATE_NOOP("Settings", "Default")},
                                {"round", QT_TRANSLATE_NOOP("Settings", "Round")},
                                {"custom", QT_TRANSLATE_NOOP("Settings", "Custom")}},
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Corner radius"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Rounding of panels, cards and buttons.")});
    defs.push_back({.key = keys::CornerRadiusMultiplier,
                    .type = Type::Real,
                    .defaultValue = real(1.0),
                    .min = 0.0,
                    .max = 2.0,
                    .step = 0.05,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Corner radius multiplier"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Scales every corner radius."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "×"),
                    .visibleWhen = Condition{keys::CornerRadius, {"custom"}}});
    defs.push_back({.key = keys::CoverArtShape,
                    .type = Type::Enum,
                    .defaultValue = str("rounded"),
                    .options = {{"rounded", QT_TRANSLATE_NOOP("Settings", "Rounded")},
                                {"square", QT_TRANSLATE_NOOP("Settings", "Square")},
                                {"circle", QT_TRANSLATE_NOOP("Settings", "Circle")}},
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Cover art shape"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Shape of artwork in lists and the player.")});

    // --- Appearance: text and scale --------------------------------------------------------------
    defs.push_back({.key = keys::UiScale,
                    .type = Type::Int,
                    .defaultValue = integer(100),
                    .min = 75,
                    .max = 200,
                    .step = 5,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Interface zoom"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Scales the whole interface."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "%")});
    defs.push_back({.key = keys::FontScale,
                    .type = Type::Int,
                    .defaultValue = integer(100),
                    .min = 80,
                    .max = 150,
                    .step = 5,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Text size"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Scales text relative to the interface zoom."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "%")});
    defs.push_back({.key = keys::HeadingFont,
                    .type = Type::String,
                    .defaultValue = str(""),
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Heading font"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Font family for titles. Empty uses Manrope."),
                    .flags = flag::Advanced});
    defs.push_back({.key = keys::BodyFont,
                    .type = Type::String,
                    .defaultValue = str(""),
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Body font"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Font family for interface text. Empty uses Inter."),
                    .flags = flag::Advanced});
    defs.push_back({.key = keys::MonoFont,
                    .type = Type::String,
                    .defaultValue = str(""),
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Monospace font"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Font family for timings and technical details. Empty uses JetBrains Mono."),
                    .flags = flag::Advanced});
    defs.push_back({.key = keys::IconWeight,
                    .type = Type::Enum,
                    .defaultValue = str("regular"),
                    .options = {{"light", QT_TRANSLATE_NOOP("Settings", "Light")},
                                {"regular", QT_TRANSLATE_NOOP("Settings", "Regular")},
                                {"bold", QT_TRANSLATE_NOOP("Settings", "Bold")}},
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Icon weight"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Stroke thickness of icons.")});

    // --- Performance -----------------------------------------------------------------------------
    defs.push_back({.key = keys::GpuAcceleration,
                    .type = Type::Enum,
                    .defaultValue = str("auto"),
                    .options = {{"auto", QT_TRANSLATE_NOOP("Settings", "Auto")},
                                {"on", QT_TRANSLATE_NOOP("Settings", "On")},
                                {"off", QT_TRANSLATE_NOOP("Settings", "Off")}},
                    .category = Performance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Hardware acceleration"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings",
                        "Auto uses the GPU unless it is missing, a software rasterizer or known to be unreliable. "
                        "Off renders on the CPU and disables blur effects."),
                    .flags = flag::RequiresRestart,
                    .cliAlias = "gpu"});
    defs.push_back({.key = keys::GraphicsApi,
                    .type = Type::Enum,
                    .defaultValue = str("auto"),
                    .options = {{"auto", QT_TRANSLATE_NOOP("Settings", "Auto")},
                                {"vulkan", "Vulkan", false},
                                {"opengl", "OpenGL", false}},
                    .category = Performance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Graphics API"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Rendering backend used for hardware acceleration."),
                    .flags = flag::RequiresRestart,
                    .visibleWhen = Condition{keys::GpuAcceleration, {"auto", "on"}},
                    .cliAlias = "graphics-api"});
    defs.push_back({.key = keys::GpuPreference,
                    .type = Type::Enum,
                    .defaultValue = str("auto"),
                    .options = {{"auto", QT_TRANSLATE_NOOP("Settings", "Auto")},
                                {"integrated", QT_TRANSLATE_NOOP("Settings", "Integrated (power saving)")},
                                {"discrete", QT_TRANSLATE_NOOP("Settings", "Discrete (high performance)")}},
                    .category = Performance,
                    .label = QT_TRANSLATE_NOOP("Settings", "GPU preference"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Which GPU to use on systems with more than one."),
                    .flags = flag::RequiresRestart | flag::Advanced,
                    .visibleWhen = Condition{keys::GpuAcceleration, {"auto", "on"}}});
    defs.push_back({.key = keys::VSync,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Performance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Vertical sync"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Synchronise frames with the display to avoid tearing."),
                    .flags = flag::RequiresRestart | flag::Advanced});
    defs.push_back({.key = keys::FrameRateCap,
                    .type = Type::Enum,
                    .defaultValue = str("auto"),
                    .options = {{"auto", QT_TRANSLATE_NOOP("Settings", "Display refresh rate")},
                                {"30", "30", false},
                                {"60", "60", false},
                                {"120", "120", false},
                                {"144", "144", false},
                                {"unlimited", QT_TRANSLATE_NOOP("Settings", "Unlimited")}},
                    .category = Performance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Frame rate limit"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Upper bound for animation frame rate."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "FPS"),
                    .flags = flag::Advanced});
    defs.push_back({.key = keys::BatterySaver,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Performance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Reduce GPU usage on battery"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Pause the animated backdrop and lower effect quality when on battery power.")});

    return defs;
}

}  // namespace

const Schema& Schema::builtin()
{
    static const Schema schema(builtinSections(), builtinDefinitions());
    return schema;
}

}  // namespace noctara::settings
