#include "theme/Tokens.hpp"

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
        glass.refraction = 0.055;
        glass.thickness = 18.0;
        glass.dispersion = 0.012;
        glass.specular = 0.55;
        glass.fresnel = 0.35;
        glass.rimDarkening = 0.25;
        glass.saturationLift = 0.18;
        glass.brightnessLift = 0.04;
    } else {
        glass.refraction = 0.045;
        glass.thickness = 16.0;
        glass.dispersion = 0.006;
        glass.specular = 0.75;
        glass.fresnel = 0.22;
        glass.rimDarkening = 0.10;
        glass.saturationLift = 0.10;
        glass.brightnessLift = 0.0;
    }
    return glass;
}

}  // namespace noctara::theme
