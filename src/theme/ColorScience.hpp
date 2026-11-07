#pragma once

#include <cstdint>

// Pure colour math (no Qt): sRGB <-> linear <-> OKLab/OKLCH, gamut mapping and WCAG contrast.
// Everything uses double precision and no fast-math so results are deterministic across builds.
namespace noctara::color {

struct Srgb {  // gamma-encoded, nominal range 0..1
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    friend bool operator==(const Srgb&, const Srgb&) = default;
};

struct LinearRgb {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};

struct OkLab {
    double L = 0.0;  // NOLINT(readability-identifier-naming): conventional notation
    double a = 0.0;
    double b = 0.0;
};

struct OkLch {
    double L = 0.0;  // NOLINT(readability-identifier-naming)
    double C = 0.0;  // NOLINT(readability-identifier-naming)
    double h = 0.0;  // degrees, [0, 360)
};

struct Rgba {
    Srgb rgb;
    double alpha = 1.0;
    friend bool operator==(const Rgba&, const Rgba&) = default;
};

[[nodiscard]] double srgbToLinear(double channel) noexcept;
[[nodiscard]] double linearToSrgb(double channel) noexcept;
[[nodiscard]] LinearRgb toLinear(Srgb color) noexcept;
[[nodiscard]] Srgb toSrgb(LinearRgb color) noexcept;

[[nodiscard]] OkLab toOkLab(LinearRgb color) noexcept;
[[nodiscard]] LinearRgb toLinear(OkLab color) noexcept;
[[nodiscard]] OkLch toOkLch(OkLab color) noexcept;
[[nodiscard]] OkLab toOkLab(OkLch color) noexcept;

[[nodiscard]] OkLab toOkLab(Srgb color) noexcept;
[[nodiscard]] OkLch toOkLch(Srgb color) noexcept;

[[nodiscard]] bool inGamut(LinearRgb color, double epsilon = 1e-6) noexcept;

// Maps an OKLCH colour into sRGB by reducing chroma at constant lightness and hue (binary search),
// then clamps residual error. This preserves the perceived hue, unlike channel clipping.
[[nodiscard]] Srgb toSrgbGamutMapped(OkLch color) noexcept;
[[nodiscard]] Srgb clamp(Srgb color) noexcept;

// WCAG 2.x relative luminance and contrast ratio (1..21).
[[nodiscard]] double relativeLuminance(Srgb color) noexcept;
[[nodiscard]] double contrastRatio(Srgb a, Srgb b) noexcept;
// The same ratio from two relative luminances, for callers that test many colours against one background.
[[nodiscard]] double contrastRatioOfLuminance(double la, double lb) noexcept;

// Source-over compositing as performed by the scene graph (gamma-encoded space, straight alpha).
[[nodiscard]] Srgb composite(Rgba foreground, Srgb background) noexcept;

// Perceptual interpolation (OKLab) of colour and linear interpolation of alpha.
[[nodiscard]] Rgba mix(const Rgba& from, const Rgba& to, double t) noexcept;

[[nodiscard]] std::uint32_t toArgb32(const Rgba& color) noexcept;

}  // namespace noctara::color
