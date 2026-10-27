#include "theme/Tokens.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace noctara::theme {

namespace {

QColor toQColor(const color::Rgba& c)
{
    return QColor::fromRgbF(static_cast<float>(c.rgb.r), static_cast<float>(c.rgb.g), static_cast<float>(c.rgb.b),
                            static_cast<float>(c.alpha));
}

}  // namespace

ThemeColors ThemeColors::fromPalette(const Palette& palette)
{
    ThemeColors colors;
#define NOCTARA_CONVERT_ROLE(name) colors.name = toQColor(palette.name);
    NOCTARA_PALETTE_ROLES(NOCTARA_CONVERT_ROLE)
#undef NOCTARA_CONVERT_ROLE
    return colors;
}

SpacingTokens SpacingTokens::scaled(double factor) const
{
    SpacingTokens out = *this;
    for (qreal* value :
         {&out.xxs, &out.xs, &out.sm, &out.md, &out.lg, &out.xl, &out.xxl, &out.xxxl, &out.controlHeight,
          &out.controlHeightSmall, &out.iconSize, &out.iconSizeSmall, &out.iconSizeLarge, &out.navRailWidth,
          &out.navRailWidthExpanded, &out.nowPlayingHeight, &out.rowHeight, &out.pageMargin, &out.contentMaxWidth}) {
        *value *= factor;
    }
    return out;
}

RadiusTokens RadiusTokens::scaled(double factor) const
{
    RadiusTokens out = *this;
    if (out.button < out.pill) {
        out.button *= factor;
    }
    out.xs *= factor;
    out.sm *= factor;
    out.md *= factor;
    out.lg *= factor;
    return out;
}

ElevationTokens ElevationTokens::scaled(double factor) const
{
    ElevationTokens out = *this;
    for (qreal* value :
         {&out.baseBlur, &out.baseOffset, &out.raisedBlur, &out.raisedOffset, &out.overlayBlur, &out.overlayOffset}) {
        *value *= factor;
    }
    return out;
}

GlassTokens glassTokensFor(Scheme scheme)
{
    GlassTokens glass;
    if (scheme == Scheme::Dark) {
        glass.refraction = 16.0;
        glass.thickness = 18.0;
        glass.dispersion = 0.18;
        glass.specular = 0.55;
        glass.fresnel = 0.35;
        glass.rimDarkening = 0.25;
        glass.saturationLift = 0.18;
        glass.brightnessLift = 0.04;
    } else {
        glass.refraction = 13.0;
        glass.thickness = 16.0;
        glass.dispersion = 0.08;
        glass.specular = 0.75;
        glass.fresnel = 0.22;
        glass.rimDarkening = 0.10;
        glass.saturationLift = 0.10;
        glass.brightnessLift = 0.0;
    }
    return glass;
}

double BlurPlan::radius() const noexcept
{
    if (iterations > 0) {
        // Each Dual Kawase level doubles the reach of the taps.
        return downsample * offset * std::exp2(iterations);
    }
    return downsample * sigma * 3.0;
}

BlurPlan blurPlan(QStringView style, double strength, int quality)
{
    const int q = std::clamp(quality, 0, 2);
    const double s = std::clamp(strength, 0.0, 1.0);
    // Capture resolution per quality: low 1/4, medium 1/3, high 1/2 of the window.
    static constexpr std::array<double, 3> kDownsample{4.0, 3.0, 2.0};
    BlurPlan plan;
    if (style == u"kawase" || style == u"liquid") {
        // Liquid Glass keeps the background clearer: the lens effects carry the look.
        const double effective = style == u"liquid" ? s * 0.6 : s;
        const int highIterations = std::clamp(1 + static_cast<int>(std::lround(effective * 4.0)), 1, 5);
        const int iterations = q == 0 ? std::max(1, highIterations - 1) : highIterations;
        const double highOffset = 1.0 + effective;
        const double highRadius = kDownsample[2] * highOffset * std::exp2(highIterations);
        plan.downsample = kDownsample.at(static_cast<std::size_t>(q));
        plan.iterations = iterations;
        // Same radius as the high-quality plan, within the offsets that stay artefact-free.
        plan.offset = std::clamp(highRadius / (plan.downsample * std::exp2(iterations)), 0.5, 3.0);
    } else if (style == u"gaussian") {
        const double sigmaPixels = 2.0 + s * 38.0;
        static constexpr std::array<double, 3> kMaxSigmaTexels{3.0, 5.0, 8.0};
        plan.downsample = std::max(kDownsample.at(static_cast<std::size_t>(q)),
                                   sigmaPixels / kMaxSigmaTexels.at(static_cast<std::size_t>(q)));
        plan.sigma = sigmaPixels / plan.downsample;
    }
    return plan;
}

}  // namespace noctara::theme
