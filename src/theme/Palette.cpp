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
constexpr double kAlphaStep = 0.01;

// Midnight: near-black blue-violet base lit by soft aurora light. Base/raised glass is a light,
// accent-tinted veil at low opacity (it lifts what is behind it); overlays (drawers, popups) are dark glass.
constexpr SchemeTones kDarkTones{
    .neutralHue = 272.0,
    .neutralChroma = 0.022,
    .neutralHueBlend = 0.2,
    .backdropL = 0.11,
    .backdropMinL = 0.07,
    .backdropMaxL = 0.32,
    .surfaceL = {0.90, 0.90, 0.19},
    .surfaceAlpha = {0.10, 0.17, 0.86},
    .solidL = {0.17, 0.20, 0.215},
    .textL = {0.955, 0.80, 0.66},
    .accentMinL = 0.64,
    .accentMaxL = 0.80,
    .accentMaxC = 0.17,
    .containerL = 0.30,
    .containerChromaScale = 0.55,
    .onContainerL = 0.94,
    .glowAlpha = 0.85,
    .tintAccentChroma = 0.045,
    .selectionAlpha = 0.28,
    .shadowL = 0.02,
    .shadowChroma = 0.01,
    .shadowAlpha = 0.65,
    .rimHighlightAlpha = 0.18,
    .rimShadowAlpha = 0.35,
    .waveformUnplayedAlpha = 0.20,
    .waveformBufferedAlpha = 0.36,
    .skeletonL = 0.23,
    .skeletonHighlightL = 0.30,
    .hoverAlpha = 0.07,
    .pressedAlpha = 0.12,
    .scrimAlpha = 0.5,
};

// Frost: pale cool base with washed pastel aurora; milky white glass, dark ink, accent-tinted shadows.
constexpr SchemeTones kLightTones{
    .neutralHue = 268.0,
    .neutralChroma = 0.018,
    .neutralHueBlend = 0.2,
    .backdropL = 0.95,
    .backdropMinL = 0.84,
    .backdropMaxL = 0.98,
    .surfaceL = {0.995, 0.995, 0.985},
    .surfaceAlpha = {0.52, 0.66, 0.86},
    .solidL = {0.965, 0.98, 0.99},
    .textL = {0.24, 0.40, 0.50},
    .accentMinL = 0.44,
    .accentMaxL = 0.56,
    .accentMaxC = 0.19,
    .containerL = 0.90,
    .containerChromaScale = 0.35,
    .onContainerL = 0.30,
    .glowAlpha = 0.5,
    .tintAccentChroma = 0.02,
    .selectionAlpha = 0.20,
    .shadowL = 0.38,
    .shadowChroma = 0.06,
    .shadowAlpha = 0.18,
    .rimHighlightAlpha = 0.85,
    .rimShadowAlpha = 0.18,
    .waveformUnplayedAlpha = 0.20,
    .waveformBufferedAlpha = 0.34,
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

// Contrast of a possibly translucent foreground as drawn: composited over each background first.
double minContrast(Rgba fg, std::span<const Srgb> backgrounds)
{
    double worst = 21.0;
    for (const Srgb& bg : backgrounds) {
        worst = std::min(worst, color::contrastRatio(color::composite(fg, bg), bg));
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

// Opaque colours that text and icons may end up on: each glass tint over both backdrop bounds and
// over the palette's own backdrop (a user theme may move it outside the bounds), the solid surfaces,
// and the backdrops themselves.
std::vector<Srgb> worstBackgrounds(const Palette& p, const SchemeTones& tones)
{
    std::vector<Srgb> out;
    const Srgb lo = gray(tones.backdropMinL);
    const Srgb hi = gray(tones.backdropMaxL);
    const Srgb base = p.backdrop.rgb;
    for (const Rgba* surface : {&p.surfaceBase, &p.surfaceRaised, &p.surfaceOverlay}) {
        out.push_back(color::composite(*surface, lo));
        out.push_back(color::composite(*surface, hi));
        out.push_back(color::composite(*surface, base));
    }
    for (const Rgba* solid : {&p.solidBase, &p.solidRaised, &p.solidOverlay}) {
        out.push_back(solid->rgb);
    }
    out.push_back(lo);
    out.push_back(hi);
    out.push_back(base);
    return out;
}

// Makes a role readable as drawn: a translucent one first gains opacity (its colour kept), then the
// lightness moves in `direction` until the target ratio holds.
void enforceRole(Rgba& role, std::span<const Srgb> backgrounds, double required, Scheme direction)
{
    if (minContrast(role, backgrounds) + 1e-9 >= required) {
        return;
    }
    while (role.alpha < 1.0 && minContrast(role, backgrounds) + 1e-9 < required) {
        role.alpha = std::min(1.0, role.alpha + kAlphaStep);
    }
    if (minContrast(role, backgrounds) + 1e-9 < required) {
        role.alpha = 1.0;
        role.rgb = color::toSrgbGamutMapped(enforce(color::toOkLch(role.rgb), backgrounds, required, direction));
    }
}

}  // namespace

const SchemeTones& defaultTones(Scheme scheme) noexcept
{
    return scheme == Scheme::Dark ? kDarkTones : kLightTones;
}

OkLch defaultSeed() noexcept
{
    return {0.62, 0.17, 275.0};  // indigo, between violet and blue
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
    // "Glass tint": 0 % is the clearest glass, 100 % the densest; 20 % equals the former 0 % (config v6).
    const double opacityScale = 0.16 + 1.2 * std::clamp(request.glassOpacity, 0.0, 1.0);
    const auto surfaceAlpha = [&](std::size_t level) {
        return std::min(0.97, t.surfaceAlpha.at(level) * opacityScale);
    };

    Palette p;
    p.backdrop = rgba({t.backdropL, nc * 1.2, nh});
    const double backdropDelta = scheme == Scheme::Dark ? 0.035 : 0.03;
    p.backdropTop =
        rgba({t.backdropL + backdropDelta, achromatic ? nc : std::min(seed.C, 0.06), achromatic ? nh : seed.h});
    p.backdropBottom = rgba({t.backdropL - backdropDelta, nc, nh});
    p.scrim = scheme == Scheme::Dark ? rgba({0.08, nc, nh}, t.scrimAlpha) : rgba({0.99, nc * 0.5, nh}, t.scrimAlpha);

    // The glass veil is tinted toward the accent (the artifact's "white 64% + accent").
    const double tintChroma = achromatic ? nc : std::min(seed.C * 0.4, t.tintAccentChroma);
    const double tintHue = achromatic ? nh : seed.h;
    p.surfaceBase = rgba({t.surfaceL[0], tintChroma, tintHue}, surfaceAlpha(0));
    p.surfaceRaised = rgba({t.surfaceL[1], tintChroma, tintHue}, surfaceAlpha(1));
    p.surfaceOverlay = rgba({t.surfaceL[2], nc, nh}, surfaceAlpha(2));
    p.glassTint = p.surfaceBase;
    p.solidBase = rgba({t.solidL[0], nc, nh});
    p.solidRaised = rgba({t.solidL[1], nc, nh});
    p.solidOverlay = rgba({t.solidL[2], nc, nh});

    p.edge = scheme == Scheme::Dark ? rgba({1.0, 0.0, 0.0}, 0.16) : rgba({1.0, 0.0, 0.0}, 0.75);
    p.sheen = scheme == Scheme::Dark ? rgba({1.0, 0.0, 0.0}, 0.07) : rgba({1.0, 0.0, 0.0}, 0.45);
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

    // Accent: seed hue, tone clamped to the scheme's range, chroma capped, then contrast-enforced. The accent
    // itself is used for fills, icons and selection (graphical, 3:1) so it can stay saturated; small accent
    // text uses accentText, which meets 4.5:1.
    OkLch accent{std::clamp(seed.L, t.accentMinL, t.accentMaxL), std::min(seed.C, t.accentMaxC), seed.h};
    std::vector<Srgb> accentBackgrounds = backgrounds;
    if (scheme == Scheme::Light) {
        accentBackgrounds.push_back(gray(1.0));  // onAccent is white in Frost
    }
    accent = enforce(accent, backgrounds, kContrastGraphic, scheme);
    if (scheme == Scheme::Light) {
        accent = enforce(accent, accentBackgrounds, kContrastText, scheme);  // white on-accent text
    }
    p.accentText = rgba(enforce(accent, backgrounds, kContrastText, scheme));
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

    // Companion colours from the artifact: a lighter partner hue (gradients, aurora) and a deep shade.
    const OkLch accentLight{std::clamp(accent.L - (scheme == Scheme::Dark ? 0.08 : -0.06), 0.0, 1.0),
                            std::min(accent.C * 0.95, t.accentMaxC), std::fmod(accent.h + 40.0, 360.0)};
    p.accentLight = rgba(accentLight);
    p.accentDeep = scheme == Scheme::Dark
                       ? rgba({0.2, std::min(accent.C * 0.5, 0.08), std::fmod(accent.h + 352.0, 360.0)})
                       : rgba({0.86, std::min(accent.C * 0.35, 0.06), accent.h});
    const double glowAlpha = t.glowAlpha * std::clamp(request.glowIntensity, 0.0, 1.0);
    p.glow = scheme == Scheme::Dark ? rgba(accent, glowAlpha) : rgba({0.72, accent.C, accent.h}, glowAlpha);
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

Palette enforceReadable(const Palette& palette, Scheme scheme, const SchemeTones& tones)
{
    Palette p = palette;
    const std::vector<Srgb> backgrounds = worstBackgrounds(p, tones);
    const auto fix = [&](Rgba& role, double required) {
        enforceRole(role, backgrounds, required, scheme);
    };
    fix(p.textPrimary, kContrastText);
    fix(p.textSecondary, kContrastText);
    fix(p.textTertiary, kContrastText);
    fix(p.iconPrimary, kContrastGraphic);
    fix(p.iconSecondary, kContrastGraphic);
    fix(p.accent, kContrastGraphic);
    fix(p.accentText, kContrastText);
    fix(p.focusRing, kContrastGraphic);
    fix(p.waveformPlayed, kContrastGraphic);
    fix(p.danger, kContrastText);
    fix(p.warning, kContrastText);
    fix(p.success, kContrastText);
    // Text on the accent: away from the accent states, towards whichever end reads better.
    const Srgb accentStates[] = {p.accent.rgb, p.accentHover.rgb, p.accentPressed.rgb};
    const Scheme away = color::relativeLuminance(p.accent.rgb) > 0.18 ? Scheme::Light : Scheme::Dark;
    enforceRole(p.onAccent, accentStates, kContrastText, away);
    return p;
}

std::vector<ContrastResult> checkContrast(const Palette& p, Scheme /*scheme*/, const SchemeTones& tones)
{
    const std::vector<Srgb> backgrounds = worstBackgrounds(p, tones);
    std::vector<ContrastResult> results;
    const auto against = [&](std::string_view role, const Rgba& fg, double required) {
        results.push_back({role, minContrast(fg, backgrounds), required});
    };
    against("textPrimary", p.textPrimary, kContrastText);
    against("textSecondary", p.textSecondary, kContrastText);
    against("textTertiary", p.textTertiary, kContrastText);
    against("iconPrimary", p.iconPrimary, kContrastGraphic);
    against("iconSecondary", p.iconSecondary, kContrastGraphic);
    against("accent", p.accent, kContrastGraphic);
    against("accentText", p.accentText, kContrastText);
    against("focusRing", p.focusRing, kContrastGraphic);
    against("waveformPlayed", p.waveformPlayed, kContrastGraphic);
    against("danger", p.danger, kContrastText);
    against("warning", p.warning, kContrastText);
    against("success", p.success, kContrastText);

    const Srgb accentStates[] = {p.accent.rgb, p.accentHover.rgb, p.accentPressed.rgb};
    results.push_back({"onAccent", minContrast(p.onAccent, accentStates), kContrastText});
    const Srgb containerBg[] = {p.accentContainer.rgb};
    results.push_back({"onAccentContainer", minContrast(p.onAccentContainer, containerBg), kContrastText});
    return results;
}

}  // namespace noctara::theme
