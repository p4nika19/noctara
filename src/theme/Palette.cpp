#include "theme/Palette.hpp"

#include <algorithm>
#include <cmath>
#include <span>

namespace noctara::theme {

namespace {

using color::OkLch;
using color::Rgba;
using color::Srgb;

constexpr double kAchromaticChroma = 0.02;
constexpr double kToneStep = 0.0025;

// Midnight: blue-violet tinted, never pure black. Glass is dark and moderately opaque.
constexpr SchemeTones kDarkTones{
    .neutralHue = 285.0,
    .neutralChroma = 0.028,
    .neutralHueBlend = 0.35,
    .backdropL = 0.17,
    .backdropMinL = 0.10,
    .backdropMaxL = 0.34,
    .surfaceL = {0.25, 0.29, 0.33},
    .surfaceAlpha = {0.50, 0.60, 0.72},
    .solidL = {0.21, 0.25, 0.29},
    .textL = {0.96, 0.84, 0.72},
    .accentMinL = 0.68,
    .accentMaxL = 0.82,
    .accentMaxC = 0.17,
    .containerL = 0.34,
    .containerChromaScale = 0.55,
    .onContainerL = 0.92,
    .glowAlpha = 0.38,
    .selectionAlpha = 0.28,
    .shadowL = 0.06,
    .shadowChroma = 0.02,
    .shadowAlpha = 0.45,
    .rimHighlightAlpha = 0.16,
    .rimShadowAlpha = 0.30,
    .waveformUnplayedAlpha = 0.30,
    .waveformBufferedAlpha = 0.48,
    .skeletonL = 0.31,
    .skeletonHighlightL = 0.39,
    .hoverAlpha = 0.07,
    .pressedAlpha = 0.13,
    .scrimAlpha = 0.45,
};

// Frost: milky white glass over a pale, cool, washed backdrop. Softer, accent-tinted shadows.
constexpr SchemeTones kLightTones{
    .neutralHue = 250.0,
    .neutralChroma = 0.016,
    .neutralHueBlend = 0.30,
    .backdropL = 0.94,
    .backdropMinL = 0.86,
    .backdropMaxL = 0.98,
    .surfaceL = {0.985, 0.99, 0.995},
    .surfaceAlpha = {0.56, 0.68, 0.80},
    .solidL = {0.965, 0.98, 0.99},
    .textL = {0.22, 0.38, 0.48},
    .accentMinL = 0.42,
    .accentMaxL = 0.56,
    .accentMaxC = 0.19,
    .containerL = 0.90,
    .containerChromaScale = 0.35,
    .onContainerL = 0.30,
    .glowAlpha = 0.22,
    .selectionAlpha = 0.20,
    .shadowL = 0.38,
    .shadowChroma = 0.06,
    .shadowAlpha = 0.16,
    .rimHighlightAlpha = 0.80,
    .rimShadowAlpha = 0.16,
    .waveformUnplayedAlpha = 0.24,
    .waveformBufferedAlpha = 0.40,
    .skeletonL = 0.90,
    .skeletonHighlightL = 0.96,
    .hoverAlpha = 0.05,
    .pressedAlpha = 0.10,
    .scrimAlpha = 0.55,
};

Rgba rgba(OkLch c, double alpha = 1.0)
{
    return {color::toSrgbGamutMapped(c), alpha};
}

double blendHue(double from, double to, double t)
{
    const double delta = std::fmod(to - from + 540.0, 360.0) - 180.0;
    double hue = from + delta * t;
    if (hue < 0.0) {
        hue += 360.0;
    }
    return std::fmod(hue, 360.0);
}

Srgb gray(double lightness)
{
    return color::toSrgbGamutMapped(OkLch{lightness, 0.0, 0.0});
}

double minContrast(Srgb fg, std::span<const Srgb> backgrounds)
{
    double worst = 21.0;
    for (const Srgb& bg : backgrounds) {
        worst = std::min(worst, color::contrastRatio(fg, bg));
    }
    return worst;
}

// Moves lightness away from the backgrounds until the target ratio holds (or L saturates).
OkLch enforce(OkLch fg, std::span<const Srgb> backgrounds, double target, Scheme scheme)
{
    const double direction = scheme == Scheme::Dark ? 1.0 : -1.0;
    for (int i = 0; i < 400; ++i) {
        if (minContrast(color::toSrgbGamutMapped(fg), backgrounds) >= target) {
            break;
        }
        const double next = std::clamp(fg.L + direction * kToneStep, 0.0, 1.0);
        if (next == fg.L) {
            break;
        }
        fg.L = next;
    }
    return fg;
}

// Opaque colours that text and icons may end up on: each glass tint over both backdrop bounds,
// the solid surfaces, and the backdrop bounds themselves.
std::vector<Srgb> worstBackgrounds(const Palette& p, const SchemeTones& tones)
{
    std::vector<Srgb> out;
    const Srgb lo = gray(tones.backdropMinL);
    const Srgb hi = gray(tones.backdropMaxL);
    for (const Rgba* surface : {&p.surfaceBase, &p.surfaceRaised, &p.surfaceOverlay}) {
        out.push_back(color::composite(*surface, lo));
        out.push_back(color::composite(*surface, hi));
    }
    for (const Rgba* solid : {&p.solidBase, &p.solidRaised, &p.solidOverlay}) {
        out.push_back(solid->rgb);
    }
    out.push_back(lo);
    out.push_back(hi);
    return out;
}

}  // namespace

const SchemeTones& defaultTones(Scheme scheme) noexcept
{
    return scheme == Scheme::Dark ? kDarkTones : kLightTones;
}

OkLch defaultSeed() noexcept
{
    return {0.66, 0.15, 285.0};  // calm violet
}

Palette mix(const Palette& from, const Palette& to, double t)
{
    Palette out;
#define NOCTARA_MIX_ROLE(name) out.name = color::mix(from.name, to.name, t);
    NOCTARA_PALETTE_ROLES(NOCTARA_MIX_ROLE)
#undef NOCTARA_MIX_ROLE
    return out;
}

Palette generatePalette(const PaletteRequest& request)
{
    const SchemeTones& t = request.tones != nullptr ? *request.tones : defaultTones(request.scheme);
    const Scheme scheme = request.scheme;
    const OkLch seed = request.seed;
    const bool achromatic = seed.C < kAchromaticChroma;
    const double nh = achromatic ? t.neutralHue : blendHue(t.neutralHue, seed.h, t.neutralHueBlend);
    const double nc = t.neutralChroma;
    const double opacityScale = 0.4 + 1.2 * std::clamp(request.glassOpacity, 0.0, 1.0);
    const auto surfaceAlpha = [&](std::size_t level) {
        return std::min(0.97, t.surfaceAlpha.at(level) * opacityScale);
    };

    Palette p;
    p.backdrop = rgba({t.backdropL, nc * 1.2, nh});
    const double backdropDelta = scheme == Scheme::Dark ? 0.06 : 0.03;
    p.backdropTop =
        rgba({t.backdropL + backdropDelta, achromatic ? nc : std::min(seed.C, 0.06), achromatic ? nh : seed.h});
    p.backdropBottom = rgba({t.backdropL - backdropDelta, nc, nh});
    p.scrim = scheme == Scheme::Dark ? rgba({0.08, nc, nh}, t.scrimAlpha) : rgba({0.99, nc * 0.5, nh}, t.scrimAlpha);

    p.surfaceBase = rgba({t.surfaceL[0], nc, nh}, surfaceAlpha(0));
    p.surfaceRaised = rgba({t.surfaceL[1], nc, nh}, surfaceAlpha(1));
    p.surfaceOverlay = rgba({t.surfaceL[2], nc, nh}, surfaceAlpha(2));
    p.glassTint = rgba({t.surfaceL[0], achromatic ? nc : std::min(seed.C * 0.35, 0.05), achromatic ? nh : seed.h},
                       surfaceAlpha(0));
    p.solidBase = rgba({t.solidL[0], nc, nh});
    p.solidRaised = rgba({t.solidL[1], nc, nh});
    p.solidOverlay = rgba({t.solidL[2], nc, nh});

    if (scheme == Scheme::Dark) {
        p.rimHighlight = rgba({1.0, 0.0, 0.0}, t.rimHighlightAlpha);
        p.rimShadow = rgba({0.05, nc, nh}, t.rimShadowAlpha);
        p.shadow = rgba({t.shadowL, t.shadowChroma, nh}, t.shadowAlpha);
    } else {
        p.rimHighlight = rgba({1.0, 0.0, 0.0}, t.rimHighlightAlpha);
        p.rimShadow = rgba({0.55, nc * 2.0, nh}, t.rimShadowAlpha);
        // Light-mode shadows are tinted by the accent instead of pure black.
        p.shadow = rgba({t.shadowL, t.shadowChroma, achromatic ? nh : seed.h}, t.shadowAlpha);
    }

    const std::vector<Srgb> backgrounds = worstBackgrounds(p, t);

    const OkLch textPrimary = enforce({t.textL[0], nc * 0.5, nh}, backgrounds, kContrastText, scheme);
    const OkLch textSecondary = enforce({t.textL[1], nc * 0.6, nh}, backgrounds, kContrastText, scheme);
    const OkLch textTertiary = enforce({t.textL[2], nc * 0.6, nh}, backgrounds, kContrastText, scheme);
    p.textPrimary = rgba(textPrimary);
    p.textSecondary = rgba(textSecondary);
    p.textTertiary = rgba(textTertiary);
    p.iconPrimary = p.textPrimary;
    p.iconSecondary = rgba(enforce({t.textL[1], nc * 0.6, nh}, backgrounds, kContrastGraphic, scheme));
    p.divider = rgba(textPrimary, 0.10);
    p.outline = rgba(textPrimary, 0.18);
    p.stateHover = rgba(textPrimary, t.hoverAlpha);
    p.statePressed = rgba(textPrimary, t.pressedAlpha);

    // Accent: seed hue, tone clamped to the scheme's range, chroma capped, then contrast-enforced.
    OkLch accent{std::clamp(seed.L, t.accentMinL, t.accentMaxL), std::min(seed.C, t.accentMaxC), seed.h};
    std::vector<Srgb> accentBackgrounds = backgrounds;
    if (scheme == Scheme::Light) {
        accentBackgrounds.push_back(gray(1.0));  // onAccent is white in Frost
    }
    accent = enforce(accent, accentBackgrounds, kContrastText, scheme);
    const double stateDelta = scheme == Scheme::Dark ? 0.04 : -0.04;
    const OkLch accentHover{std::clamp(accent.L + stateDelta, 0.0, 1.0), accent.C, accent.h};
    const OkLch accentPressed{std::clamp(accent.L - stateDelta, 0.0, 1.0), accent.C, accent.h};
    p.accent = rgba(accent);
    p.accentHover = rgba(accentHover);
    p.accentPressed = rgba(accentPressed);

    const Srgb accentStates[] = {p.accent.rgb, p.accentHover.rgb, p.accentPressed.rgb};
    const OkLch onAccentStart = scheme == Scheme::Dark ? OkLch{0.16, nc, nh} : OkLch{1.0, 0.0, 0.0};
    // On-accent moves away from the accent: darker in Midnight, lighter (already white) in Frost.
    p.onAccent = rgba(
        enforce(onAccentStart, accentStates, kContrastText, scheme == Scheme::Dark ? Scheme::Light : Scheme::Dark));

    p.accentContainer = rgba({t.containerL, accent.C * t.containerChromaScale, accent.h});
    const Srgb containerBg[] = {p.accentContainer.rgb};
    p.onAccentContainer = rgba(enforce({t.onContainerL, accent.C * 0.4, accent.h}, containerBg, kContrastText, scheme));

    p.glow = scheme == Scheme::Dark ? rgba(accent, t.glowAlpha) : rgba({0.78, accent.C * 0.8, accent.h}, t.glowAlpha);
    p.selection = rgba(accent, t.selectionAlpha);
    p.focusRing = rgba(enforce({accent.L, accent.C, accent.h}, backgrounds, kContrastGraphic, scheme));

    p.waveformPlayed = p.accent;
    p.waveformUnplayed = rgba(textPrimary, t.waveformUnplayedAlpha);
    p.waveformBuffered = rgba(textPrimary, t.waveformBufferedAlpha);
    p.skeletonBase = rgba({t.skeletonL, nc, nh}, 0.7);
    p.skeletonHighlight = rgba({t.skeletonHighlightL, nc, nh}, 0.7);

    const auto status = [&](double hue) {
        return rgba(enforce({(t.accentMinL + t.accentMaxL) / 2.0, 0.16, hue}, backgrounds, kContrastText, scheme));
    };
    p.danger = status(25.0);
    p.warning = status(75.0);
    p.success = status(150.0);
    return p;
}

std::vector<ContrastResult> checkContrast(const Palette& p, Scheme /*scheme*/, const SchemeTones& tones)
{
    const std::vector<Srgb> backgrounds = worstBackgrounds(p, tones);
    std::vector<ContrastResult> results;
    const auto against = [&](std::string_view role, const Rgba& fg, double required) {
        results.push_back({role, minContrast(fg.rgb, backgrounds), required});
    };
    against("textPrimary", p.textPrimary, kContrastText);
    against("textSecondary", p.textSecondary, kContrastText);
    against("textTertiary", p.textTertiary, kContrastText);
    against("iconPrimary", p.iconPrimary, kContrastGraphic);
    against("iconSecondary", p.iconSecondary, kContrastGraphic);
    against("accent", p.accent, kContrastText);
    against("focusRing", p.focusRing, kContrastGraphic);
    against("waveformPlayed", p.waveformPlayed, kContrastGraphic);
    against("danger", p.danger, kContrastText);
    against("warning", p.warning, kContrastText);
    against("success", p.success, kContrastText);

    const Srgb accentStates[] = {p.accent.rgb, p.accentHover.rgb, p.accentPressed.rgb};
    results.push_back({"onAccent", minContrast(p.onAccent.rgb, accentStates), kContrastText});
    results.push_back(
        {"onAccentContainer", color::contrastRatio(p.onAccentContainer.rgb, p.accentContainer.rgb), kContrastText});
    return results;
}

}  // namespace noctara::theme
