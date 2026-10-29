#pragma once

#include "theme/ColorScience.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace noctara::theme {

enum class Scheme : std::uint8_t { Dark, Light };

// Every semantic colour role. Shared by both schemes; only values differ.
// X(name) expands for each role, keeping the struct, interpolation and the QML gadget in sync.
#define NOCTARA_PALETTE_ROLES(X)                                                                                       \
    X(backdrop)                                                                                                        \
    X(backdropTop)                                                                                                     \
    X(backdropBottom)                                                                                                  \
    X(scrim)                                                                                                           \
    X(glassTint)                                                                                                       \
    X(surfaceBase)                                                                                                     \
    X(surfaceRaised)                                                                                                   \
    X(surfaceOverlay)                                                                                                  \
    X(solidBase)                                                                                                       \
    X(solidRaised)                                                                                                     \
    X(solidOverlay)                                                                                                    \
    X(rimHighlight)                                                                                                    \
    X(rimShadow)                                                                                                       \
    X(shadow)                                                                                                          \
    X(textPrimary)                                                                                                     \
    X(textSecondary)                                                                                                   \
    X(textTertiary)                                                                                                    \
    X(iconPrimary)                                                                                                     \
    X(iconSecondary)                                                                                                   \
    X(divider)                                                                                                         \
    X(outline)                                                                                                         \
    X(stateHover)                                                                                                      \
    X(statePressed)                                                                                                    \
    X(accent)                                                                                                          \
    X(accentText)                                                                                                      \
    X(accentLight)                                                                                                     \
    X(accentDeep)                                                                                                      \
    X(edge)                                                                                                            \
    X(sheen)                                                                                                           \
    X(accentHover)                                                                                                     \
    X(accentPressed)                                                                                                   \
    X(onAccent)                                                                                                        \
    X(accentContainer)                                                                                                 \
    X(onAccentContainer)                                                                                               \
    X(glow)                                                                                                            \
    X(selection)                                                                                                       \
    X(focusRing)                                                                                                       \
    X(waveformPlayed)                                                                                                  \
    X(waveformUnplayed)                                                                                                \
    X(waveformBuffered)                                                                                                \
    X(skeletonBase)                                                                                                    \
    X(skeletonHighlight)                                                                                               \
    X(danger)                                                                                                          \
    X(warning)                                                                                                         \
    X(success)

struct Palette {
#define NOCTARA_DECLARE_ROLE(name) color::Rgba name;
    NOCTARA_PALETTE_ROLES(NOCTARA_DECLARE_ROLE)
#undef NOCTARA_DECLARE_ROLE
    friend bool operator==(const Palette&, const Palette&) = default;
};

[[nodiscard]] Palette mix(const Palette& from, const Palette& to, double t);

// Scheme-specific tone targets: the token *values* that distinguish Midnight from Frost.
struct SchemeTones {
    double neutralHue;       // hue of neutrals when the seed is achromatic
    double neutralChroma;    // chroma of tinted neutrals
    double neutralHueBlend;  // 0 = neutralHue, 1 = seed hue
    double backdropL;        // base backdrop lightness
    double backdropMinL;     // legibility bounds of the (cover-art) backdrop after scrim/wash
    double backdropMaxL;
    std::array<double, 3> surfaceL;      // glass tint lightness: base, raised, overlay
    std::array<double, 3> surfaceAlpha;  // glass tint opacity at the default "glass tint" setting
    std::array<double, 3> solidL;        // opaque surfaces (Solid mode, reduce transparency)
    std::array<double, 3> textL;         // primary, secondary, tertiary
    double accentMinL;
    double accentMaxL;
    double accentMaxC;
    double containerL;
    double containerChromaScale;
    double onContainerL;
    double glowAlpha;         // glow opacity at full glow intensity
    double tintAccentChroma;  // chroma of the accent-tinted glass veil
    double selectionAlpha;
    double shadowL;
    double shadowChroma;
    double shadowAlpha;
    double rimHighlightAlpha;
    double rimShadowAlpha;
    double waveformUnplayedAlpha;
    double waveformBufferedAlpha;
    double skeletonL;
    double skeletonHighlightL;
    double hoverAlpha;
    double pressedAlpha;
    double scrimAlpha;
};

[[nodiscard]] const SchemeTones& defaultTones(Scheme scheme) noexcept;

// Default seed when no artwork/static accent is available, or the artwork is achromatic.
[[nodiscard]] color::OkLch defaultSeed() noexcept;

struct PaletteRequest {
    color::OkLch seed = defaultSeed();
    Scheme scheme = Scheme::Dark;
    double glassOpacity = 0.5;           // "Glass tint" setting, 0..1; 0.5 = token defaults
    double glowIntensity = 0.3;          // "Glow intensity" setting, 0..1
    const SchemeTones* tones = nullptr;  // defaults when null
};

// Generates the full palette. Text, icons, accents and focus rings are adjusted in tone until they meet
// WCAG AA against the worst-case glass background (glass tint over both backdrop lightness bounds).
[[nodiscard]] Palette generatePalette(const PaletteRequest& request);

struct ContrastResult {
    std::string_view role;
    double ratio;
    double required;
    [[nodiscard]] bool passes() const noexcept { return ratio + 1e-9 >= required; }
};

// Contrast of every foreground role against every background it is drawn on (worst case).
[[nodiscard]] std::vector<ContrastResult> checkContrast(const Palette& palette, Scheme scheme,
                                                        const SchemeTones& tones);

// Moves every text, icon and accent role that falls below WCAG AA against the glass it sits on to a readable
// tone (hue kept). Used after a user theme overrode colours, when "Enforce readable contrast" is on.
[[nodiscard]] Palette enforceReadable(const Palette& palette, Scheme scheme, const SchemeTones& tones);

inline constexpr double kContrastText = 4.5;
inline constexpr double kContrastGraphic = 3.0;

}  // namespace noctara::theme
