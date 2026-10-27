#include "theme/Theme.hpp"

#include "core/Log.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"

#include <QColor>
#include <QEasingCurve>
#include <QGuiApplication>
#include <QStyleHints>

#include <algorithm>

namespace noctara::theme {

namespace keys = settings::keys;

namespace {

int styleCost(const QString& style)
{
    if (style == u"liquid") {
        return 3;
    }
    if (style == u"gaussian") {
        return 2;
    }
    if (style == u"kawase") {
        return 1;
    }
    return 0;
}

QFont makeFont(const QStringList& families, double pixelSize, int weight, bool tabular = false,
               double letterSpacing = 0.0)
{
    QFont font;
    font.setFamilies(families);
    font.setPixelSize(std::max(1, static_cast<int>(std::lround(pixelSize))));
    font.setWeight(static_cast<QFont::Weight>(weight));
    font.setHintingPreference(QFont::PreferNoHinting);
    if (tabular) {
        font.setFeature(QFont::Tag("tnum"), 1);
    }
    if (letterSpacing != 0.0) {
        font.setLetterSpacing(QFont::AbsoluteSpacing, letterSpacing);
    }
    return font;
}

}  // namespace

Theme::Theme(settings::SettingsStore& settings, FontFamilies bundledFonts, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_bundledFonts(std::move(bundledFonts))
{
    m_crossfade.setStartValue(0.0);
    m_crossfade.setEndValue(1.0);
    m_crossfade.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_crossfade, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) { applyProgress(value.toDouble()); });

    connect(&m_settings, &settings::SettingsStore::valueChanged, this, &Theme::onSettingChanged);
    if (const auto* hints = QGuiApplication::styleHints()) {
        connect(hints, &QStyleHints::colorSchemeChanged, this, [this] { updateScheme(true); });
    }

    updateMotion();
    updateMetrics();
    updateFonts();
    updateScheme(false);
    updateGlass();
}

QString Theme::schemeName() const
{
    return dark() ? QStringLiteral("dark") : QStringLiteral("light");
}

void Theme::onSettingChanged(const QString& key)
{
    const auto is = [&key](std::string_view candidate) {
        return key == QLatin1StringView(candidate.data(), static_cast<qsizetype>(candidate.size()));
    };
    if (is(keys::ThemeMode)) {
        updateScheme(true);
    } else if (is(keys::AccentAdaptive) || is(keys::AccentColor) || is(keys::EnforceContrast)) {
        updatePalette(m_motion.accent);
    } else if (is(keys::GlowIntensity) || is(keys::GlassOpacity)) {
        updatePalette(0);
        updateGlass();
    } else if (is(keys::ButtonShape) || is(keys::Density)) {
        updateMetrics();
    } else if (is(keys::ColorTransition)) {
        updateMotion();
    } else if (is(keys::UiScale) || is(keys::CornerRadius) || is(keys::CornerRadiusMultiplier) ||
               is(keys::IconWeight) || is(keys::CoverArtShape)) {
        updateMetrics();
        updateFonts();
    } else if (is(keys::FontScale) || is(keys::HeadingFont) || is(keys::BodyFont) || is(keys::MonoFont)) {
        updateFonts();
    } else if (is(keys::ReduceMotion) || is(keys::AnimationSpeed)) {
        updateMotion();
        updateGlass();
    } else if (is(keys::GlassStyle) || is(keys::GlassBlurStrength) || is(keys::GlassGrain) ||
               is(keys::ReduceTransparency) || is(keys::GlowCover) || is(keys::GlowControls) ||
               is(keys::GlowProgress) || is(keys::ButtonMaterial) || is(keys::BackdropStyle) ||
               is(keys::BackdropAnimate)) {
        updateGlass();
    }
}

double Theme::glassOpacity() const
{
    return std::clamp(m_settings.realValue(keys::GlassOpacity) + keys::GlassOpacityBaseline, 0.0, 1.0);
}

int Theme::animated(int durationMs) const
{
    return m_motion.enabled && !m_motion.reduced ? durationMs : 0;
}

color::OkLch Theme::currentSeed() const
{
    if (m_settings.boolValue(keys::AccentAdaptive)) {
        if (m_artworkSeed) {
            return *m_artworkSeed;
        }
    } else {
        const QColor seed = QColor::fromString(m_settings.stringValue(keys::AccentColor));
        if (seed.isValid()) {
            return color::toOkLch(color::Srgb{static_cast<double>(seed.redF()), static_cast<double>(seed.greenF()),
                                              static_cast<double>(seed.blueF())});
        }
    }
    return defaultSeed();
}

void Theme::updateScheme(bool animate)
{
    const QString mode = m_settings.stringValue(keys::ThemeMode);
    Scheme next = Scheme::Dark;
    if (mode == u"light") {
        next = Scheme::Light;
    } else if (mode == u"system") {
        const auto* hints = QGuiApplication::styleHints();
        next = hints != nullptr && hints->colorScheme() == Qt::ColorScheme::Light ? Scheme::Light : Scheme::Dark;
    }
    const bool changed = next != m_scheme;
    m_scheme = next;
    if (changed) {
        Q_EMIT schemeChanged();
        updateGlass();
    }
    updatePalette(animate ? m_motion.theme : 0);
}

void Theme::updatePalette(int durationMs)
{
    const PaletteRequest request{.seed = currentSeed(),
                                 .scheme = m_scheme,
                                 .glassOpacity = glassOpacity(),
                                 .glowIntensity = m_settings.realValue(keys::GlowIntensity)};
    const Palette target = generatePalette(request);
    const auto results = checkContrast(target, m_scheme, defaultTones(m_scheme));
    m_contrastOk = std::ranges::all_of(results, [](const ContrastResult& r) { return r.passes(); });
    if (!m_contrastOk) {
        for (const auto& r : results) {
            if (!r.passes()) {
                qCWarning(lcTheme).nospace()
                    << "contrast below AA for " << QByteArrayView(r.role.data(), static_cast<qsizetype>(r.role.size()))
                    << ": " << r.ratio << " < " << r.required;
            }
        }
    }

    if (target == m_target && m_crossfade.state() != QAbstractAnimation::Running) {
        return;
    }
    m_target = target;
    if (const ThemeColors targetColors = ThemeColors::fromPalette(target); !(targetColors == m_targetColors)) {
        m_targetColors = targetColors;
        Q_EMIT targetColorsChanged();
    }
    m_crossfade.stop();
    const int duration = animated(durationMs);
    if (duration <= 0) {
        applyProgress(1.0);
        m_from = m_target;
        return;
    }
    m_from = m_displayed;
    m_crossfade.setDuration(duration);
    m_crossfade.start();
}

void Theme::applyProgress(double t)
{
    m_displayed = t >= 1.0 ? m_target : mix(m_from, m_target, t);
    m_colors = ThemeColors::fromPalette(m_displayed);
    Q_EMIT colorsChanged();
}

void Theme::updateMetrics()
{
    m_scale = static_cast<qreal>(m_settings.intValue(keys::UiScale)) / 100.0;
    // Density scales the gaps between things, not the size of controls.
    const QString density = m_settings.stringValue(keys::Density);
    const double densityFactor = density == u"compact" ? 0.82 : (density == u"airy" ? 1.18 : 1.0);
    SpacingTokens space = SpacingTokens{}.scaled(m_scale);
    for (qreal* gap : {&space.sm, &space.md, &space.lg, &space.xl, &space.xxl, &space.xxxl, &space.pageMargin}) {
        *gap *= densityFactor;
    }
    m_space = space;
    m_elevation = ElevationTokens{}.scaled(m_scale);

    const QString cornerMode = m_settings.stringValue(keys::CornerRadius);
    double radiusFactor = 1.0;
    if (cornerMode == u"sharp") {
        radiusFactor = 0.3;
    } else if (cornerMode == u"round") {
        radiusFactor = 1.4;
    } else if (cornerMode == u"custom") {
        radiusFactor = m_settings.realValue(keys::CornerRadiusMultiplier);
    }
    RadiusTokens radius;
    const QString buttonShape = m_settings.stringValue(keys::ButtonShape);
    radius.button = buttonShape == u"soft" ? 14.0 : (buttonShape == u"sharp" ? 6.0 : radius.pill);
    m_radius = radius.scaled(m_scale * radiusFactor);

    const QString weight = m_settings.stringValue(keys::IconWeight);
    m_iconStroke = weight == u"light" ? 1.25 : (weight == u"bold" ? 2.0 : 1.5);
    m_coverShape = m_settings.stringValue(keys::CoverArtShape);
    Q_EMIT metricsChanged();
}

void Theme::updateFonts()
{
    const double factor = m_scale * static_cast<double>(m_settings.intValue(keys::FontScale)) / 100.0;
    const QString heading = FontLoader::resolve(m_settings.stringValue(keys::HeadingFont), m_bundledFonts.heading);
    const QString body = FontLoader::resolve(m_settings.stringValue(keys::BodyFont), m_bundledFonts.body);
    const QString mono = FontLoader::resolve(m_settings.stringValue(keys::MonoFont), m_bundledFonts.mono);
    const QStringList headingChain = FontLoader::fallbackChain(heading, m_bundledFonts.heading);
    const QStringList bodyChain = FontLoader::fallbackChain(body, m_bundledFonts.body);
    const QStringList monoChain = FontLoader::fallbackChain(mono, m_bundledFonts.mono);

    m_font.display = makeFont(headingChain, 38 * factor, 800, false, -0.5 * factor);
    m_font.headline = makeFont(headingChain, 30 * factor, 800, false, -0.3 * factor);
    m_font.title = makeFont(headingChain, 21 * factor, 700);
    m_font.subtitle = makeFont(headingChain, 17 * factor, 700);
    m_font.body = makeFont(bodyChain, 14 * factor, 400);
    m_font.bodyStrong = makeFont(bodyChain, 14 * factor, 650);
    m_font.label = makeFont(bodyChain, 13 * factor, 600);
    m_font.caption = makeFont(bodyChain, 12 * factor, 450);
    m_font.overline = makeFont(bodyChain, 11 * factor, 650, false, 0.8 * factor);
    m_font.eyebrow = makeFont(bodyChain, 11 * factor, 800, false, 1.7 * factor);
    m_font.eyebrow.setCapitalization(QFont::AllUppercase);
    m_font.groupTitle = makeFont(monoChain, 11 * factor, 500, false, 1.5 * factor);
    m_font.groupTitle.setCapitalization(QFont::AllUppercase);
    m_font.mono = makeFont(monoChain, 12.5 * factor, 450, true);
    m_font.monoSmall = makeFont(monoChain, 11 * factor, 450, true);
    m_font.numeric = makeFont(bodyChain, 14 * factor, 450, true);
    Q_EMIT fontsChanged();
}

void Theme::updateMotion()
{
    const double speed = m_settings.realValue(keys::AnimationSpeed);
    MotionTokens motion;
    motion.reduced = m_settings.boolValue(keys::ReduceMotion);
    motion.enabled = speed > 0.0;
    const auto scaled = [&](int base) {
        return motion.enabled ? static_cast<int>(std::lround(base * speed)) : 0;
    };
    motion.fast = scaled(motion.fast);
    motion.normal = scaled(motion.normal);
    motion.slow = scaled(motion.slow);
    // Accent/palette crossfade speed ("Colour transitions").
    const QString transition = m_settings.stringValue(keys::ColorTransition);
    motion.accent = scaled(transition == u"fast" ? 350 : (transition == u"slow" ? 2600 : 1200));
    motion.theme = scaled(motion.theme);
    motion.backdrop = scaled(motion.backdrop);
    motion.shimmer = motion.reduced ? 0 : scaled(motion.shimmer);
    motion.easingStandard = QEasingCurve::OutExpo;
    // Decelerating curves only: nothing travels past its target and springs back.
    motion.easingEmphasized = QEasingCurve::OutQuint;
    motion.easingExit = QEasingCurve::InCubic;
    motion.overshoot = 0.0;
    m_motion = motion;
    Q_EMIT motionChanged();
}

void Theme::updateGlass()
{
    GlassTokens glass = glassTokensFor(m_scheme);
    glass.requestedStyle = m_settings.stringValue(keys::GlassStyle);
    glass.blurAvailable = m_blurAvailable;
    glass.blurStrength = m_settings.realValue(keys::GlassBlurStrength);
    glass.tint = glassOpacity();
    // Frost uses less grain than Midnight at the same setting.
    glass.grain = std::clamp(m_settings.realValue(keys::GlassGrain) + keys::GlassGrainBaseline, 0.0, 1.0) *
                  (m_scheme == Scheme::Dark ? 1.0 : 0.6);
    glass.buttons = m_settings.stringValue(keys::ButtonMaterial);
    glass.backdrop = m_settings.stringValue(keys::BackdropStyle);
    glass.backdropAnimated = m_settings.boolValue(keys::BackdropAnimate) && m_blurAvailable &&
                             !m_settings.boolValue(keys::ReduceMotion) && m_backdropAnimationAllowed;
    glass.glow = m_settings.realValue(keys::GlowIntensity);
    glass.glowCover = m_settings.boolValue(keys::GlowCover);
    glass.glowControls = m_settings.boolValue(keys::GlowControls);
    glass.glowProgress = m_settings.boolValue(keys::GlowProgress);
    if (m_settings.boolValue(keys::ReduceTransparency) && (glass.buttons != u"clear")) {
        glass.buttons = QStringLiteral("solid");
    }

    QString style = glass.requestedStyle;
    if (!m_blurAvailable || m_settings.boolValue(keys::ReduceTransparency)) {
        style = QStringLiteral("solid");
    } else if (!m_styleCap.isEmpty() && styleCost(m_styleCap) < styleCost(style)) {
        style = m_styleCap;
    }
    glass.style = style;
    glass.translucent = style != u"solid";
    glass.blurred = styleCost(style) > 0;
    glass.quality = m_quality;
    const BlurPlan plan = blurPlan(style, glass.blurStrength, m_quality);
    glass.blurDownsample = plan.downsample;
    glass.blurIterations = plan.iterations;
    glass.blurOffset = plan.offset;
    glass.blurSigma = plan.sigma;
    if (glass == m_glass) {
        return;
    }
    m_glass = std::move(glass);
    Q_EMIT glassChanged();
}

void Theme::setArtworkSeed(std::optional<color::OkLch> seed)
{
    const bool same =
        seed.has_value() == m_artworkSeed.has_value() &&
        (!seed || (seed->L == m_artworkSeed->L && seed->C == m_artworkSeed->C && seed->h == m_artworkSeed->h));
    if (same) {
        return;
    }
    m_artworkSeed = seed;
    if (m_settings.boolValue(keys::AccentAdaptive)) {
        updatePalette(m_motion.accent);
    }
}

void Theme::setBlurAvailable(bool available)
{
    m_blurAvailable = available;
    updateGlass();
}

void Theme::setGlassLimits(int quality, const QString& styleCap, bool backdropAnimation)
{
    m_quality = std::clamp(quality, 0, 2);
    m_styleCap = styleCap;
    m_backdropAnimationAllowed = backdropAnimation;
    updateGlass();
}

}  // namespace noctara::theme
