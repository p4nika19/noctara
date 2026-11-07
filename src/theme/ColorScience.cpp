#include "theme/ColorScience.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace noctara::color {

namespace {

constexpr double kDegPerRad = 180.0 / std::numbers::pi;

double clamp01(double value) noexcept
{
    return std::clamp(value, 0.0, 1.0);
}

}  // namespace

double srgbToLinear(double channel) noexcept
{
    return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
}

double linearToSrgb(double channel) noexcept
{
    return channel <= 0.0031308 ? channel * 12.92 : 1.055 * std::pow(channel, 1.0 / 2.4) - 0.055;
}

LinearRgb toLinear(Srgb color) noexcept
{
    return {srgbToLinear(color.r), srgbToLinear(color.g), srgbToLinear(color.b)};
}

Srgb toSrgb(LinearRgb color) noexcept
{
    return {linearToSrgb(color.r), linearToSrgb(color.g), linearToSrgb(color.b)};
}

// Matrices from Björn Ottosson, "A perceptual color space for image processing" (2020).
OkLab toOkLab(LinearRgb c) noexcept
{
    const double l = std::cbrt(0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b);
    const double m = std::cbrt(0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b);
    const double s = std::cbrt(0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b);
    return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
            1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

LinearRgb toLinear(OkLab c) noexcept
{
    const double l_ = c.L + 0.3963377774 * c.a + 0.2158037573 * c.b;
    const double m_ = c.L - 0.1055613458 * c.a - 0.0638541728 * c.b;
    const double s_ = c.L - 0.0894841775 * c.a - 1.2914855480 * c.b;
    const double l = l_ * l_ * l_;
    const double m = m_ * m_ * m_;
    const double s = s_ * s_ * s_;
    return {4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
            -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
            -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s};
}

OkLch toOkLch(OkLab c) noexcept
{
    double hue = std::atan2(c.b, c.a) * kDegPerRad;
    if (hue < 0.0) {
        hue += 360.0;
    }
    return {c.L, std::hypot(c.a, c.b), hue};
}

OkLab toOkLab(OkLch c) noexcept
{
    const double radians = c.h / kDegPerRad;
    return {c.L, c.C * std::cos(radians), c.C * std::sin(radians)};
}

OkLab toOkLab(Srgb color) noexcept
{
    return toOkLab(toLinear(color));
}

OkLch toOkLch(Srgb color) noexcept
{
    return toOkLch(toOkLab(color));
}

bool inGamut(LinearRgb c, double epsilon) noexcept
{
    return c.r >= -epsilon && c.r <= 1.0 + epsilon && c.g >= -epsilon && c.g <= 1.0 + epsilon && c.b >= -epsilon &&
           c.b <= 1.0 + epsilon;
}

Srgb clamp(Srgb color) noexcept
{
    return {clamp01(color.r), clamp01(color.g), clamp01(color.b)};
}

Srgb toSrgbGamutMapped(OkLch color) noexcept
{
    color.L = clamp01(color.L);
    color.C = std::max(0.0, color.C);
    if (const LinearRgb linear = toLinear(toOkLab(color)); inGamut(linear)) {
        return clamp(toSrgb(linear));
    }
    double low = 0.0;
    double high = color.C;
    for (int i = 0; i < 24; ++i) {
        const double mid = (low + high) / 2.0;
        if (inGamut(toLinear(toOkLab(OkLch{color.L, mid, color.h})))) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return clamp(toSrgb(toLinear(toOkLab(OkLch{color.L, low, color.h}))));
}

double relativeLuminance(Srgb color) noexcept
{
    const LinearRgb linear = toLinear(clamp(color));
    return 0.2126 * linear.r + 0.7152 * linear.g + 0.0722 * linear.b;
}

double contrastRatio(Srgb a, Srgb b) noexcept
{
    return contrastRatioOfLuminance(relativeLuminance(a), relativeLuminance(b));
}

double contrastRatioOfLuminance(double la, double lb) noexcept
{
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

Srgb composite(Rgba foreground, Srgb background) noexcept
{
    const double a = clamp01(foreground.alpha);
    const auto over = [a](double fg, double bg) {
        return fg * a + bg * (1.0 - a);
    };
    return {over(foreground.rgb.r, background.r), over(foreground.rgb.g, background.g),
            over(foreground.rgb.b, background.b)};
}

Rgba mix(const Rgba& from, const Rgba& to, double t) noexcept
{
    t = clamp01(t);
    const OkLab a = toOkLab(from.rgb);
    const OkLab b = toOkLab(to.rgb);
    const OkLab mixed{a.L + (b.L - a.L) * t, a.a + (b.a - a.a) * t, a.b + (b.b - a.b) * t};
    return {clamp(toSrgb(toLinear(mixed))), from.alpha + (to.alpha - from.alpha) * t};
}

std::uint32_t toArgb32(const Rgba& color) noexcept
{
    const auto channel = [](double value) {
        return static_cast<std::uint32_t>(std::lround(clamp01(value) * 255.0));
    };
    return (channel(color.alpha) << 24U) | (channel(color.rgb.r) << 16U) | (channel(color.rgb.g) << 8U) |
           channel(color.rgb.b);
}

Rgba fromArgb32(std::uint32_t argb) noexcept
{
    const auto channel = [argb](unsigned shift) {
        return static_cast<double>((argb >> shift) & 0xffU) / 255.0;
    };
    return {{channel(16), channel(8), channel(0)}, channel(24)};
}

}  // namespace noctara::color
