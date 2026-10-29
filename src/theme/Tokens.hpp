#pragma once

#include "theme/Palette.hpp"

#include <QColor>
#include <QFont>
#include <QObject>
#include <QString>

// Design tokens exposed to QML as immutable value objects (Q_GADGET): one property per token group, so
// a theme/accent change is a single notification rather than dozens of property updates.
namespace noctara::theme {

class ThemeColors {
    Q_GADGET
    Q_PROPERTY(QColor backdrop MEMBER backdrop CONSTANT)
    Q_PROPERTY(QColor backdropTop MEMBER backdropTop CONSTANT)
    Q_PROPERTY(QColor backdropBottom MEMBER backdropBottom CONSTANT)
    Q_PROPERTY(QColor scrim MEMBER scrim CONSTANT)
    Q_PROPERTY(QColor glassTint MEMBER glassTint CONSTANT)
    Q_PROPERTY(QColor surfaceBase MEMBER surfaceBase CONSTANT)
    Q_PROPERTY(QColor surfaceRaised MEMBER surfaceRaised CONSTANT)
    Q_PROPERTY(QColor surfaceOverlay MEMBER surfaceOverlay CONSTANT)
    Q_PROPERTY(QColor solidBase MEMBER solidBase CONSTANT)
    Q_PROPERTY(QColor solidRaised MEMBER solidRaised CONSTANT)
    Q_PROPERTY(QColor solidOverlay MEMBER solidOverlay CONSTANT)
    Q_PROPERTY(QColor rimHighlight MEMBER rimHighlight CONSTANT)
    Q_PROPERTY(QColor rimShadow MEMBER rimShadow CONSTANT)
    Q_PROPERTY(QColor shadow MEMBER shadow CONSTANT)
    Q_PROPERTY(QColor textPrimary MEMBER textPrimary CONSTANT)
    Q_PROPERTY(QColor textSecondary MEMBER textSecondary CONSTANT)
    Q_PROPERTY(QColor textTertiary MEMBER textTertiary CONSTANT)
    Q_PROPERTY(QColor iconPrimary MEMBER iconPrimary CONSTANT)
    Q_PROPERTY(QColor iconSecondary MEMBER iconSecondary CONSTANT)
    Q_PROPERTY(QColor divider MEMBER divider CONSTANT)
    Q_PROPERTY(QColor outline MEMBER outline CONSTANT)
    Q_PROPERTY(QColor stateHover MEMBER stateHover CONSTANT)
    Q_PROPERTY(QColor statePressed MEMBER statePressed CONSTANT)
    Q_PROPERTY(QColor accent MEMBER accent CONSTANT)
    Q_PROPERTY(QColor accentText MEMBER accentText CONSTANT)  // accent for small text (4.5:1)
    Q_PROPERTY(QColor accentLight MEMBER accentLight CONSTANT)
    Q_PROPERTY(QColor accentDeep MEMBER accentDeep CONSTANT)
    Q_PROPERTY(QColor edge MEMBER edge CONSTANT)
    Q_PROPERTY(QColor sheen MEMBER sheen CONSTANT)
    Q_PROPERTY(QColor accentHover MEMBER accentHover CONSTANT)
    Q_PROPERTY(QColor accentPressed MEMBER accentPressed CONSTANT)
    Q_PROPERTY(QColor onAccent MEMBER onAccent CONSTANT)
    Q_PROPERTY(QColor accentContainer MEMBER accentContainer CONSTANT)
    Q_PROPERTY(QColor onAccentContainer MEMBER onAccentContainer CONSTANT)
    Q_PROPERTY(QColor glow MEMBER glow CONSTANT)
    Q_PROPERTY(QColor selection MEMBER selection CONSTANT)
    Q_PROPERTY(QColor focusRing MEMBER focusRing CONSTANT)
    Q_PROPERTY(QColor waveformPlayed MEMBER waveformPlayed CONSTANT)
    Q_PROPERTY(QColor waveformUnplayed MEMBER waveformUnplayed CONSTANT)
    Q_PROPERTY(QColor waveformBuffered MEMBER waveformBuffered CONSTANT)
    Q_PROPERTY(QColor skeletonBase MEMBER skeletonBase CONSTANT)
    Q_PROPERTY(QColor skeletonHighlight MEMBER skeletonHighlight CONSTANT)
    Q_PROPERTY(QColor danger MEMBER danger CONSTANT)
    Q_PROPERTY(QColor warning MEMBER warning CONSTANT)
    Q_PROPERTY(QColor success MEMBER success CONSTANT)

public:
#define NOCTARA_DECLARE_QCOLOR(name) QColor name;
    NOCTARA_PALETTE_ROLES(NOCTARA_DECLARE_QCOLOR)
#undef NOCTARA_DECLARE_QCOLOR

    [[nodiscard]] static ThemeColors fromPalette(const Palette& palette);
    friend bool operator==(const ThemeColors&, const ThemeColors&) = default;
};

class SpacingTokens {
    Q_GADGET
    Q_PROPERTY(qreal xxs MEMBER xxs CONSTANT)
    Q_PROPERTY(qreal xs MEMBER xs CONSTANT)
    Q_PROPERTY(qreal sm MEMBER sm CONSTANT)
    Q_PROPERTY(qreal md MEMBER md CONSTANT)
    Q_PROPERTY(qreal lg MEMBER lg CONSTANT)
    Q_PROPERTY(qreal xl MEMBER xl CONSTANT)
    Q_PROPERTY(qreal xxl MEMBER xxl CONSTANT)
    Q_PROPERTY(qreal xxxl MEMBER xxxl CONSTANT)
    // Component metrics derived from the spacing scale.
    Q_PROPERTY(qreal controlHeight MEMBER controlHeight CONSTANT)
    Q_PROPERTY(qreal controlHeightSmall MEMBER controlHeightSmall CONSTANT)
    Q_PROPERTY(qreal iconSize MEMBER iconSize CONSTANT)
    Q_PROPERTY(qreal iconSizeSmall MEMBER iconSizeSmall CONSTANT)
    Q_PROPERTY(qreal iconSizeLarge MEMBER iconSizeLarge CONSTANT)
    Q_PROPERTY(qreal navRailWidth MEMBER navRailWidth CONSTANT)
    Q_PROPERTY(qreal navRailWidthExpanded MEMBER navRailWidthExpanded CONSTANT)
    Q_PROPERTY(qreal nowPlayingHeight MEMBER nowPlayingHeight CONSTANT)
    Q_PROPERTY(qreal rowHeight MEMBER rowHeight CONSTANT)
    Q_PROPERTY(qreal pageMargin MEMBER pageMargin CONSTANT)
    Q_PROPERTY(qreal contentMaxWidth MEMBER contentMaxWidth CONSTANT)
    Q_PROPERTY(qreal focusRingWidth MEMBER focusRingWidth CONSTANT)
    Q_PROPERTY(qreal hairline MEMBER hairline CONSTANT)

public:
    qreal xxs = 2;
    qreal xs = 4;
    qreal sm = 8;
    qreal md = 12;
    qreal lg = 16;
    qreal xl = 24;
    qreal xxl = 32;
    qreal xxxl = 48;
    qreal controlHeight = 40;
    qreal controlHeightSmall = 32;
    qreal iconSize = 20;
    qreal iconSizeSmall = 16;
    qreal iconSizeLarge = 28;
    qreal navRailWidth = 72;
    qreal navRailWidthExpanded = 220;
    qreal nowPlayingHeight = 80;
    qreal rowHeight = 56;
    qreal pageMargin = 32;
    qreal contentMaxWidth = 1400;
    qreal focusRingWidth = 2;
    qreal hairline = 1;

    [[nodiscard]] SpacingTokens scaled(double factor) const;
    friend bool operator==(const SpacingTokens&, const SpacingTokens&) = default;
};

class RadiusTokens {
    Q_GADGET
    Q_PROPERTY(qreal xs MEMBER xs CONSTANT)
    Q_PROPERTY(qreal sm MEMBER sm CONSTANT)
    Q_PROPERTY(qreal md MEMBER md CONSTANT)
    Q_PROPERTY(qreal lg MEMBER lg CONSTANT)
    Q_PROPERTY(qreal pill MEMBER pill CONSTANT)
    Q_PROPERTY(qreal button MEMBER button CONSTANT)  // buttons and segmented controls ("Button shape")

public:
    qreal xs = 6;
    qreal sm = 12;
    qreal md = 20;
    qreal lg = 28;
    qreal pill = 9999;
    qreal button = 9999;

    [[nodiscard]] RadiusTokens scaled(double factor) const;
    friend bool operator==(const RadiusTokens&, const RadiusTokens&) = default;
};

class TypographyTokens {
    Q_GADGET
    Q_PROPERTY(QFont display MEMBER display CONSTANT)
    Q_PROPERTY(QFont headline MEMBER headline CONSTANT)
    Q_PROPERTY(QFont title MEMBER title CONSTANT)
    Q_PROPERTY(QFont subtitle MEMBER subtitle CONSTANT)
    Q_PROPERTY(QFont body MEMBER body CONSTANT)
    Q_PROPERTY(QFont bodyStrong MEMBER bodyStrong CONSTANT)
    Q_PROPERTY(QFont label MEMBER label CONSTANT)
    Q_PROPERTY(QFont caption MEMBER caption CONSTANT)
    Q_PROPERTY(QFont overline MEMBER overline CONSTANT)
    Q_PROPERTY(QFont eyebrow MEMBER eyebrow CONSTANT)        // small uppercase kicker above headings
    Q_PROPERTY(QFont groupTitle MEMBER groupTitle CONSTANT)  // mono uppercase group labels
    Q_PROPERTY(QFont mono MEMBER mono CONSTANT)
    Q_PROPERTY(QFont monoSmall MEMBER monoSmall CONSTANT)
    Q_PROPERTY(QFont numeric MEMBER numeric CONSTANT)

public:
    QFont display;
    QFont headline;
    QFont title;
    QFont subtitle;
    QFont body;
    QFont bodyStrong;
    QFont label;
    QFont caption;
    QFont overline;
    QFont eyebrow;
    QFont groupTitle;
    QFont mono;  // timings, bitrate, counters, hex input (tabular)
    QFont monoSmall;
    QFont numeric;  // body face with tabular numerals, for counts inside prose layouts
    friend bool operator==(const TypographyTokens&, const TypographyTokens&) = default;
};

class MotionTokens {
    Q_GADGET
    Q_PROPERTY(int fast MEMBER fast CONSTANT)
    Q_PROPERTY(int normal MEMBER normal CONSTANT)
    Q_PROPERTY(int slow MEMBER slow CONSTANT)
    Q_PROPERTY(int accent MEMBER accent CONSTANT)
    Q_PROPERTY(int theme MEMBER theme CONSTANT)
    Q_PROPERTY(int backdrop MEMBER backdrop CONSTANT)
    Q_PROPERTY(int shimmer MEMBER shimmer CONSTANT)
    Q_PROPERTY(int easingStandard MEMBER easingStandard CONSTANT)
    Q_PROPERTY(int easingEmphasized MEMBER easingEmphasized CONSTANT)
    Q_PROPERTY(int easingExit MEMBER easingExit CONSTANT)
    Q_PROPERTY(qreal overshoot MEMBER overshoot CONSTANT)
    Q_PROPERTY(bool reduced MEMBER reduced CONSTANT)
    Q_PROPERTY(bool enabled MEMBER enabled CONSTANT)

public:
    int fast = 120;
    int normal = 200;
    int slow = 320;
    int accent = 500;
    int theme = 400;
    int backdrop = 600;
    int shimmer = 1400;
    int easingStandard = 0;  // QEasingCurve::Type, filled in by Theme
    int easingEmphasized = 0;
    int easingExit = 0;
    qreal overshoot = 1.2;  // OutBack amplitude for press "squash"/spring feel
    bool reduced = false;
    bool enabled = true;
    friend bool operator==(const MotionTokens&, const MotionTokens&) = default;
};

class ElevationTokens {
    Q_GADGET
    Q_PROPERTY(qreal baseBlur MEMBER baseBlur CONSTANT)
    Q_PROPERTY(qreal baseOffset MEMBER baseOffset CONSTANT)
    Q_PROPERTY(qreal raisedBlur MEMBER raisedBlur CONSTANT)
    Q_PROPERTY(qreal raisedOffset MEMBER raisedOffset CONSTANT)
    Q_PROPERTY(qreal overlayBlur MEMBER overlayBlur CONSTANT)
    Q_PROPERTY(qreal overlayOffset MEMBER overlayOffset CONSTANT)

public:
    qreal baseBlur = 16;
    qreal baseOffset = 4;
    qreal raisedBlur = 28;
    qreal raisedOffset = 10;
    qreal overlayBlur = 44;
    qreal overlayOffset = 18;

    [[nodiscard]] ElevationTokens scaled(double factor) const;
    friend bool operator==(const ElevationTokens&, const ElevationTokens&) = default;
};

class GlassTokens {
    Q_GADGET
    // Effective style after fallbacks: kawase | gaussian | liquid | solid.
    Q_PROPERTY(QString style MEMBER style CONSTANT)
    Q_PROPERTY(QString requestedStyle MEMBER requestedStyle CONSTANT)
    Q_PROPERTY(QString surface MEMBER surface CONSTANT)
    Q_PROPERTY(QString requestedSurface MEMBER requestedSurface CONSTANT)
    Q_PROPERTY(bool flat MEMBER flat CONSTANT)
    Q_PROPERTY(bool blurred MEMBER blurred CONSTANT)  // the shared backdrop blur pipeline is in use
    Q_PROPERTY(int quality MEMBER quality CONSTANT)   // 0 low, 1 medium, 2 high (after the governor)
    Q_PROPERTY(qreal blurDownsample MEMBER blurDownsample CONSTANT)
    Q_PROPERTY(int blurIterations MEMBER blurIterations CONSTANT)
    Q_PROPERTY(qreal blurOffset MEMBER blurOffset CONSTANT)
    Q_PROPERTY(qreal blurSigma MEMBER blurSigma CONSTANT)
    Q_PROPERTY(bool blurAvailable MEMBER blurAvailable CONSTANT)
    Q_PROPERTY(bool translucent MEMBER translucent CONSTANT)  // false in Solid / reduce transparency
    Q_PROPERTY(qreal blurStrength MEMBER blurStrength CONSTANT)
    Q_PROPERTY(qreal tint MEMBER tint CONSTANT)
    Q_PROPERTY(qreal grain MEMBER grain CONSTANT)
    Q_PROPERTY(qreal rimWidth MEMBER rimWidth CONSTANT)
    Q_PROPERTY(qreal refraction MEMBER refraction CONSTANT)
    Q_PROPERTY(qreal thickness MEMBER thickness CONSTANT)
    Q_PROPERTY(qreal dispersion MEMBER dispersion CONSTANT)
    Q_PROPERTY(qreal specular MEMBER specular CONSTANT)
    Q_PROPERTY(qreal fresnel MEMBER fresnel CONSTANT)
    Q_PROPERTY(qreal rimDarkening MEMBER rimDarkening CONSTANT)
    Q_PROPERTY(qreal saturationLift MEMBER saturationLift CONSTANT)
    Q_PROPERTY(qreal brightnessLift MEMBER brightnessLift CONSTANT)
    Q_PROPERTY(qreal lightAngle MEMBER lightAngle CONSTANT)
    Q_PROPERTY(QString buttons MEMBER buttons CONSTANT)    // frosted | liquid | tinted | clear | solid
    Q_PROPERTY(QString backdrop MEMBER backdrop CONSTANT)  // aurora | cover | dark
    Q_PROPERTY(bool backdropAnimated MEMBER backdropAnimated CONSTANT)
    Q_PROPERTY(qreal glow MEMBER glow CONSTANT)  // glow intensity 0..1
    Q_PROPERTY(bool glowCover MEMBER glowCover CONSTANT)
    Q_PROPERTY(bool glowControls MEMBER glowControls CONSTANT)
    Q_PROPERTY(bool glowProgress MEMBER glowProgress CONSTANT)

public:
    QString style = QStringLiteral("kawase");
    QString requestedStyle = QStringLiteral("kawase");
    // Look of panels, independent of the blur: frosted | liquid | flat (effective, after fallbacks).
    QString surface = QStringLiteral("frosted");
    QString requestedSurface = QStringLiteral("frosted");
    bool flat = false;
    bool blurred = true;
    int quality = 2;
    // Shared blur pipeline parameters, see blurPlan().
    qreal blurDownsample = 2.0;
    int blurIterations = 3;
    qreal blurOffset = 1.6;
    qreal blurSigma = 0.0;
    bool blurAvailable = true;
    bool translucent = true;
    qreal blurStrength = 0.6;
    qreal tint = 0.5;
    qreal grain = 0.35;
    qreal rimWidth = 1.0;
    // Liquid Glass parameters (scheme-specific values, see glassTokensFor()). Lengths are in pixels at 100% zoom.
    qreal refraction = 0.0;  // how far the rim bends the background
    qreal thickness = 0.0;   // width of the bending rim
    qreal dispersion = 0.0;  // relative colour separation of the bend
    qreal specular = 0.0;
    qreal fresnel = 0.0;
    qreal rimDarkening = 0.0;
    qreal saturationLift = 0.0;
    qreal brightnessLift = 0.0;
    qreal lightAngle = 135.0;  // degrees; light from the top-left
    QString buttons = QStringLiteral("frosted");
    QString backdrop = QStringLiteral("aurora");
    bool backdropAnimated = true;
    qreal glow = 0.3;
    bool glowCover = true;
    bool glowControls = true;
    bool glowProgress = true;
    friend bool operator==(const GlassTokens&, const GlassTokens&) = default;
};

// Scheme-specific Liquid Glass / grain defaults (Frost needs cleaner refraction and less grain).
[[nodiscard]] GlassTokens glassTokensFor(Scheme scheme);

// Parameters of the shared backdrop blur for an effective style, "Blur strength" (0..1) and quality
// (0 low, 1 medium, 2 high). The backdrop is captured at 1/downsample of the window size, then:
//   kawase, liquid  `iterations` Dual Kawase down/up passes with taps `offset` texels apart
//   gaussian        one horizontal and one vertical pass with `sigma` (in captured texels)
// Lower quality captures at a lower resolution and runs fewer passes but keeps roughly the same blur
// radius, so the governor changes the cost, not the look. Styles without blur get iterations == 0.
struct BlurPlan {
    double downsample = 1.0;
    int iterations = 0;
    double offset = 0.0;
    double sigma = 0.0;
    // Approximate blur radius in window pixels (used by tests and the Performance page).
    [[nodiscard]] double radius() const noexcept;
};
[[nodiscard]] BlurPlan blurPlan(QStringView style, double strength, int quality);

}  // namespace noctara::theme
