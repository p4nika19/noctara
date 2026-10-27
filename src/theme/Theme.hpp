#pragma once

#include "theme/FontLoader.hpp"
#include "theme/Palette.hpp"
#include "theme/Tokens.hpp"

#include <QObject>
#include <QVariantAnimation>

#include <optional>

namespace noctara::settings {
class SettingsStore;
}

namespace noctara::theme {

// The QML "Theme" singleton: every design token, derived from settings, the OS colour scheme and
// the current accent seed. Colour changes crossfade via a single progress value.
class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool dark READ dark NOTIFY schemeChanged)
    Q_PROPERTY(QString scheme READ schemeName NOTIFY schemeChanged)
    Q_PROPERTY(noctara::theme::ThemeColors colors READ colors NOTIFY colorsChanged)
    // Final colours of the current transition. Used by rasterised assets (icons) that crossfade
    // themselves instead of being re-rendered on every animation frame.
    Q_PROPERTY(noctara::theme::ThemeColors targetColors READ targetColors NOTIFY targetColorsChanged)
    Q_PROPERTY(noctara::theme::SpacingTokens space READ space NOTIFY metricsChanged)
    Q_PROPERTY(noctara::theme::RadiusTokens radius READ radius NOTIFY metricsChanged)
    Q_PROPERTY(noctara::theme::ElevationTokens elevation READ elevation NOTIFY metricsChanged)
    Q_PROPERTY(noctara::theme::TypographyTokens font READ font NOTIFY fontsChanged)
    Q_PROPERTY(noctara::theme::MotionTokens motion READ motion NOTIFY motionChanged)
    Q_PROPERTY(noctara::theme::GlassTokens glass READ glass NOTIFY glassChanged)
    Q_PROPERTY(qreal scale READ scale NOTIFY metricsChanged)
    Q_PROPERTY(qreal iconStroke READ iconStroke NOTIFY metricsChanged)
    Q_PROPERTY(QString coverShape READ coverShape NOTIFY metricsChanged)
    Q_PROPERTY(bool contrastOk READ contrastOk NOTIFY colorsChanged)

public:
    Theme(settings::SettingsStore& settings, FontFamilies bundledFonts, QObject* parent = nullptr);

    [[nodiscard]] bool dark() const noexcept { return m_scheme == Scheme::Dark; }
    [[nodiscard]] Scheme scheme() const noexcept { return m_scheme; }
    [[nodiscard]] QString schemeName() const;
    [[nodiscard]] const ThemeColors& colors() const noexcept { return m_colors; }
    [[nodiscard]] const ThemeColors& targetColors() const noexcept { return m_targetColors; }
    [[nodiscard]] const Palette& palette() const noexcept { return m_displayed; }
    [[nodiscard]] const SpacingTokens& space() const noexcept { return m_space; }
    [[nodiscard]] const RadiusTokens& radius() const noexcept { return m_radius; }
    [[nodiscard]] const ElevationTokens& elevation() const noexcept { return m_elevation; }
    [[nodiscard]] const TypographyTokens& font() const noexcept { return m_font; }
    [[nodiscard]] const MotionTokens& motion() const noexcept { return m_motion; }
    [[nodiscard]] const GlassTokens& glass() const noexcept { return m_glass; }
    [[nodiscard]] qreal scale() const noexcept { return m_scale; }
    [[nodiscard]] qreal iconStroke() const noexcept { return m_iconStroke; }
    [[nodiscard]] const QString& coverShape() const noexcept { return m_coverShape; }
    [[nodiscard]] bool contrastOk() const noexcept { return m_contrastOk; }

    // Seed extracted from the current artwork (AccentEngine); nullopt when there is none.
    void setArtworkSeed(std::optional<color::OkLch> seed);
    // False in the software renderer: blur styles fall back to Solid.
    void setBlurAvailable(bool available);
    // Limits from the quality governor and energy saving: effects quality (0 low, 1 medium, 2 high), the most
    // expensive glass style allowed ("" = no cap) and whether the backdrop may animate.
    void setGlassLimits(int quality, const QString& styleCap, bool backdropAnimation);

Q_SIGNALS:
    void schemeChanged();
    void colorsChanged();
    void targetColorsChanged();
    void metricsChanged();
    void fontsChanged();
    void motionChanged();
    void glassChanged();

private:
    void onSettingChanged(const QString& key);
    void updateScheme(bool animate);
    void updatePalette(int durationMs);
    void updateMetrics();
    void updateFonts();
    void updateMotion();
    void updateGlass();
    void applyProgress(double t);
    [[nodiscard]] color::OkLch currentSeed() const;
    [[nodiscard]] int animated(int durationMs) const;

    settings::SettingsStore& m_settings;
    FontFamilies m_bundledFonts;

    Scheme m_scheme = Scheme::Dark;
    Palette m_from;
    Palette m_target;
    Palette m_displayed;
    ThemeColors m_colors;
    ThemeColors m_targetColors;
    QVariantAnimation m_crossfade;
    std::optional<color::OkLch> m_artworkSeed;
    bool m_contrastOk = true;

    SpacingTokens m_space;
    RadiusTokens m_radius;
    ElevationTokens m_elevation;
    TypographyTokens m_font;
    MotionTokens m_motion;
    GlassTokens m_glass;
    qreal m_scale = 1.0;
    qreal m_iconStroke = 1.5;
    QString m_coverShape = QStringLiteral("rounded");
    bool m_blurAvailable = true;
    int m_quality = 2;
    QString m_styleCap;
    bool m_backdropAnimationAllowed = true;
};

}  // namespace noctara::theme
