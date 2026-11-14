#include "TestUtil.hpp"

#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"
#include "theme/AccentEngine.hpp"
#include "theme/ColorScience.hpp"
#include "theme/Palette.hpp"
#include "theme/Theme.hpp"

#include <QImage>
#include <QPainter>
#include <QSignalSpy>

#include <algorithm>
#include <cmath>
#include <random>

using namespace noctara;
using namespace noctara::theme;
namespace keys = noctara::settings::keys;

namespace {

color::OkLch seedOf(double r, double g, double b)
{
    return color::toOkLch(color::Srgb{r, g, b});
}

struct Fixture {
    const char* name;
    color::OkLch seed;
};

// Pathological and typical seeds: neon, primaries, achromatic, extremes, pastel.
std::vector<Fixture> fixtures()
{
    return {
        {"default", defaultSeed()},
        {"neon green", seedOf(0.2, 1.0, 0.1)},
        {"pure red", seedOf(1.0, 0.0, 0.0)},
        {"pure blue", seedOf(0.0, 0.0, 1.0)},
        {"yellow", seedOf(1.0, 0.92, 0.0)},
        {"neon magenta", seedOf(1.0, 0.0, 1.0)},
        {"cyan", seedOf(0.0, 1.0, 1.0)},
        {"grayscale", seedOf(0.5, 0.5, 0.5)},
        {"all black", seedOf(0.0, 0.0, 0.0)},
        {"all white", seedOf(1.0, 1.0, 1.0)},
        {"low-saturation pastel", seedOf(0.85, 0.80, 0.88)},
        {"dark brown", seedOf(0.25, 0.15, 0.08)},
    };
}

}  // namespace

TEST_SUITE("theme.color")
{
    TEST_CASE("OKLab reference points")
    {
        const auto white = color::toOkLab(color::Srgb{1, 1, 1});
        CHECK(white.L == doctest::Approx(1.0).epsilon(1e-4));
        CHECK(std::abs(white.a) < 1e-4);
        CHECK(std::abs(white.b) < 1e-4);
        const auto black = color::toOkLab(color::Srgb{0, 0, 0});
        CHECK(black.L == doctest::Approx(0.0));
        CHECK(color::contrastRatio({0, 0, 0}, {1, 1, 1}) == doctest::Approx(21.0));
    }

    TEST_CASE("sRGB -> OKLCH -> sRGB round-trips and gamut mapping stays in gamut")
    {
        std::mt19937 rng(42);  // deterministic
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        for (int i = 0; i < 500; ++i) {
            const color::Srgb in{unit(rng), unit(rng), unit(rng)};
            const color::Srgb out = color::toSrgbGamutMapped(color::toOkLch(in));
            CHECK(out.r == doctest::Approx(in.r).epsilon(1e-5));
            CHECK(out.g == doctest::Approx(in.g).epsilon(1e-5));
            CHECK(out.b == doctest::Approx(in.b).epsilon(1e-5));

            const color::OkLch wild{unit(rng), unit(rng) * 0.5, unit(rng) * 360.0};
            CHECK(color::inGamut(color::toLinear(color::toSrgbGamutMapped(wild)), 1e-6));
        }
    }

    TEST_CASE("mix endpoints")
    {
        const color::Rgba a{{0.1, 0.2, 0.3}, 0.2};
        const color::Rgba b{{0.9, 0.5, 0.1}, 1.0};
        const auto start = color::mix(a, b, 0.0);
        const auto end = color::mix(a, b, 1.0);
        CHECK(start.rgb.r == doctest::Approx(a.rgb.r).epsilon(1e-6));
        CHECK(end.rgb.g == doctest::Approx(b.rgb.g).epsilon(1e-6));
        CHECK(end.alpha == doctest::Approx(1.0));
    }
}

TEST_SUITE("theme.palette")
{
    TEST_CASE("every role meets WCAG AA in both schemes for every fixture and glass opacity")
    {
        for (const Scheme scheme : {Scheme::Dark, Scheme::Light}) {
            for (const Fixture& fixture : fixtures()) {
                for (const double opacity : {0.0, 0.5, 1.0}) {
                    const Palette palette =
                        generatePalette({.seed = fixture.seed, .scheme = scheme, .glassOpacity = opacity});
                    for (const ContrastResult& result : checkContrast(palette, scheme, defaultTones(scheme))) {
                        INFO(fixture.name, " scheme=", scheme == Scheme::Dark ? "dark" : "light", " opacity=", opacity,
                             " role=", std::string(result.role), " ratio=", result.ratio);
                        CHECK(result.passes());
                    }
                }
            }
        }
    }

    TEST_CASE("generation is deterministic")
    {
        const PaletteRequest request{.seed = seedOf(0.3, 0.6, 0.9), .scheme = Scheme::Light};
        CHECK(generatePalette(request) == generatePalette(request));
    }

    TEST_CASE("midnight is never pure black and frost keeps glass lighter than in midnight")
    {
        const Palette dark = generatePalette({.scheme = Scheme::Dark});
        const Palette light = generatePalette({.scheme = Scheme::Light});
        const color::OkLch backdrop = color::toOkLch(dark.backdrop.rgb);
        CHECK(backdrop.L >= 0.08);                          // never black
        CHECK(backdrop.L <= 0.14);                          // but genuinely midnight-dark
        CHECK(backdrop.C >= 0.02);                          // visibly tinted
        CHECK((backdrop.h > 240.0 && backdrop.h < 285.0));  // blue
        CHECK(light.surfaceBase.alpha > dark.surfaceBase.alpha);
        CHECK(color::relativeLuminance(light.textPrimary.rgb) < color::relativeLuminance(dark.textPrimary.rgb));
    }

    TEST_CASE("translucent foreground colours are judged and enforced as drawn over the glass")
    {
        for (const Scheme scheme : {Scheme::Dark, Scheme::Light}) {
            CAPTURE(std::string(scheme == Scheme::Dark ? "dark" : "light"));
            const SchemeTones& tones = defaultTones(scheme);
            Palette palette = generatePalette({.scheme = scheme, .glassOpacity = 0.5});
            // "#66ffffff" in Midnight, "#66000000" in Frost: opaque, either would pass easily.
            const color::Srgb ink = scheme == Scheme::Dark ? color::Srgb{1.0, 1.0, 1.0} : color::Srgb{0.0, 0.0, 0.0};
            palette.textTertiary = {ink, 0.4};
            palette.iconSecondary = {ink, 0.3};
            palette.onAccent = {palette.onAccent.rgb, 0.3};

            const auto result = [](const std::vector<ContrastResult>& results, std::string_view role) {
                const auto it = std::ranges::find(results, role, &ContrastResult::role);
                REQUIRE(it != results.end());
                return *it;
            };
            const auto before = checkContrast(palette, scheme, tones);
            CHECK_FALSE(result(before, "textTertiary").passes());
            CHECK_FALSE(result(before, "iconSecondary").passes());
            CHECK_FALSE(result(before, "onAccent").passes());

            const Palette fixed = enforceReadable(palette, scheme, tones);
            for (const ContrastResult& r : checkContrast(fixed, scheme, tones)) {
                INFO("role=", std::string(r.role), " ratio=", r.ratio);
                CHECK(r.passes());
            }
            // Measured independently: the colour as composited over each worst-case background.
            const auto gray = [](double l) {
                return color::toSrgbGamutMapped({l, 0.0, 0.0});
            };
            std::vector<color::Srgb> backgrounds{gray(tones.backdropMinL), gray(tones.backdropMaxL),
                                                 fixed.solidBase.rgb, fixed.solidRaised.rgb, fixed.solidOverlay.rgb};
            for (const color::Rgba& surface : {fixed.surfaceBase, fixed.surfaceRaised, fixed.surfaceOverlay}) {
                backgrounds.push_back(color::composite(surface, gray(tones.backdropMinL)));
                backgrounds.push_back(color::composite(surface, gray(tones.backdropMaxL)));
            }
            for (const color::Srgb& bg : backgrounds) {
                CHECK(color::contrastRatio(color::composite(fixed.textTertiary, bg), bg) >= kContrastText - 1e-9);
                CHECK(color::contrastRatio(color::composite(fixed.iconSecondary, bg), bg) >= kContrastGraphic - 1e-9);
            }
        }
    }

    TEST_CASE("a theme's own backdrop colour counts as a background for checks and enforcement")
    {
        const SchemeTones& tones = defaultTones(Scheme::Dark);
        Palette palette = generatePalette({.scheme = Scheme::Dark, .glassOpacity = 0.5});
        palette.backdrop = {{0x4c / 255.0, 0x56 / 255.0, 0x6a / 255.0}, 1.0};  // lighter than backdropMaxL

        const auto failing = [](const std::vector<ContrastResult>& results) {
            return std::ranges::count_if(results, [](const ContrastResult& r) { return !r.passes(); });
        };
        CHECK(failing(checkContrast(palette, Scheme::Dark, tones)) > 0);

        const Palette fixed = enforceReadable(palette, Scheme::Dark, tones);
        for (const color::Rgba& surface : {fixed.surfaceBase, fixed.surfaceRaised, fixed.surfaceOverlay}) {
            const color::Srgb bg = color::composite(surface, fixed.backdrop.rgb);
            CHECK(color::contrastRatio(color::composite(fixed.textSecondary, bg), bg) >= kContrastText - 1e-9);
            CHECK(color::contrastRatio(color::composite(fixed.textTertiary, bg), bg) >= kContrastText - 1e-9);
        }
        CHECK(failing(checkContrast(fixed, Scheme::Dark, tones)) == 0);
    }

    TEST_CASE("a theme's light accent container gets readable on-container text when enforcing")
    {
        for (const Scheme scheme : {Scheme::Dark, Scheme::Light}) {
            CAPTURE(std::string(scheme == Scheme::Dark ? "dark" : "light"));
            const SchemeTones& tones = defaultTones(scheme);
            Palette palette = generatePalette({.scheme = scheme});
            // A light container in Midnight, a dark one in Frost: the generated on-container text vanishes on it.
            palette.accentContainer = scheme == Scheme::Dark ? color::Rgba{{0xe0 / 255.0, 0xe0 / 255.0, 1.0}, 1.0}
                                                             : color::Rgba{{0.1, 0.1, 0.2}, 1.0};
            const Palette fixed = enforceReadable(palette, scheme, tones);
            for (const ContrastResult& r : checkContrast(fixed, scheme, tones)) {
                INFO("role=", std::string(r.role), " ratio=", r.ratio);
                CHECK(r.passes());
            }
        }
    }
}

TEST_SUITE("theme.object")
{
    TEST_CASE("scheme follows the setting and glass falls back to solid without GPU blur")
    {
        settings::SettingsStore store(settings::Schema::builtin());
        REQUIRE(store.setUserValue(keys::ThemeMode, QStringLiteral("dark")).has_value());
        REQUIRE(store.setUserValue(keys::AnimationSpeed, 0.0).has_value());  // apply colours instantly
        Theme theme(store,
                    FontFamilies{QStringLiteral("Manrope"), QStringLiteral("Inter"), QStringLiteral("JetBrains Mono")});
        CHECK(theme.dark());

        QSignalSpy scheme(&theme, &Theme::schemeChanged);
        REQUIRE(store.setUserValue(keys::ThemeMode, QStringLiteral("light")).has_value());
        CHECK(scheme.count() == 1);
        CHECK_FALSE(theme.dark());
        CHECK(theme.colors().textPrimary.lightnessF() < 0.5F);

        CHECK(theme.glass().style == QStringLiteral("kawase"));
        CHECK(theme.glass().blurred);
        theme.setBlurAvailable(false);
        CHECK(theme.glass().style == QStringLiteral("solid"));
        CHECK_FALSE(theme.glass().translucent);
        CHECK_FALSE(theme.glass().blurred);
        CHECK(theme.glass().blurIterations == 0);
        CHECK_FALSE(theme.glass().backdropAnimated);
        theme.setBlurAvailable(true);
        theme.setGlassLimits(2, QStringLiteral("solid"), true);
        CHECK(theme.glass().style == QStringLiteral("solid"));
        CHECK_FALSE(theme.glass().translucent);

        REQUIRE(store.setUserValue(keys::GlassStyle, QStringLiteral("gaussian")).has_value());
        REQUIRE(store.setUserValue(keys::GlassSurface, QStringLiteral("liquid")).has_value());
        theme.setGlassLimits(1, QStringLiteral("kawase"), false);
        CHECK(theme.glass().style == QStringLiteral("kawase"));
        CHECK(theme.glass().requestedStyle == QStringLiteral("gaussian"));
        CHECK(theme.glass().surface == QStringLiteral("frosted"));  // the cap rules out Liquid Glass
        CHECK(theme.glass().requestedSurface == QStringLiteral("liquid"));
        CHECK(theme.glass().quality == 1);
        CHECK_FALSE(theme.glass().backdropAnimated);
        theme.setGlassLimits(2, QString(), true);
        CHECK(theme.glass().style == QStringLiteral("gaussian"));
        CHECK(theme.glass().surface == QStringLiteral("liquid"));
        CHECK(theme.glass().backdropAnimated);
        REQUIRE(store.setUserValue(keys::ReduceTransparency, true).has_value());
        CHECK(theme.glass().style == QStringLiteral("solid"));
        CHECK(theme.glass().surface == QStringLiteral("frosted"));  // nothing to bend without a blur
    }

    TEST_CASE("The surface style is independent of the blur: flat surfaces over a Dual Kawase blur")
    {
        settings::SettingsStore store(settings::Schema::builtin());
        Theme theme(store,
                    FontFamilies{QStringLiteral("Manrope"), QStringLiteral("Inter"), QStringLiteral("JetBrains Mono")});
        REQUIRE(store.setUserValue(keys::GlassStyle, QStringLiteral("kawase")).has_value());
        REQUIRE(store.setUserValue(keys::GlassSurface, QStringLiteral("flat")).has_value());
        CHECK(theme.glass().style == QStringLiteral("kawase"));
        CHECK(theme.glass().blurred);
        CHECK(theme.glass().flat);
        CHECK(theme.glass().surface == QStringLiteral("flat"));
        // The blur is the plain Kawase plan, not the softer one used under Liquid Glass.
        CHECK(theme.glass().blurIterations == blurPlan(u"kawase", theme.glass().blurStrength, 2).iterations);
        REQUIRE(store.setUserValue(keys::GlassStyle, QStringLiteral("solid")).has_value());
        CHECK(theme.glass().flat);  // flat opaque surfaces
        CHECK_FALSE(theme.glass().blurred);
    }

    TEST_CASE("UI zoom scales metrics and fonts; zero animation speed disables motion")
    {
        settings::SettingsStore store(settings::Schema::builtin());
        Theme theme(store,
                    FontFamilies{QStringLiteral("Manrope"), QStringLiteral("Inter"), QStringLiteral("JetBrains Mono")});
        const qreal baseSpacing = theme.space().lg;
        const int baseFont = theme.font().body.pixelSize();
        REQUIRE(store.setUserValue(keys::UiScale, QVariant::fromValue(qint64{150})).has_value());
        CHECK(theme.space().lg == doctest::Approx(baseSpacing * 1.5));
        CHECK(theme.font().body.pixelSize() == static_cast<int>(std::lround(baseFont * 1.5)));
        CHECK(theme.font().mono.featureValue(QFont::Tag("tnum")) == 1U);

        REQUIRE(store.setUserValue(keys::AnimationSpeed, 0.0).has_value());
        CHECK_FALSE(theme.motion().enabled);
        CHECK(theme.motion().normal == 0);
    }

    TEST_CASE("Blur plan: strength widens the blur, quality changes the cost but not the radius")
    {
        for (const char16_t* style : {u"kawase", u"gaussian", u"liquid"}) {
            CAPTURE(QString::fromUtf16(style));
            double previous = 0.0;
            for (double strength = 0.0; strength <= 1.0001; strength += 0.25) {
                const BlurPlan plan = blurPlan(QStringView(style), strength, 2);
                CHECK(plan.radius() >= previous);
                previous = plan.radius();
            }
            for (double strength : {0.2, 0.6, 1.0}) {
                const BlurPlan high = blurPlan(QStringView(style), strength, 2);
                const BlurPlan low = blurPlan(QStringView(style), strength, 0);
                CHECK(low.downsample >= high.downsample);
                CHECK(low.iterations <= high.iterations);
                CHECK(low.radius() == doctest::Approx(high.radius()).epsilon(0.35));
                CHECK(high.downsample >= 2.0);
            }
        }
        CHECK(blurPlan(u"kawase", 0.6, 2).iterations >= 2);
        CHECK(blurPlan(u"kawase", 1.0, 2).iterations <= 5);
        CHECK(blurPlan(u"liquid", 1.0, 2).radius() < blurPlan(u"kawase", 1.0, 2).radius());
        CHECK(blurPlan(u"gaussian", 1.0, 2).sigma <= 8.0);
        CHECK(blurPlan(u"solid", 1.0, 2).iterations == 0);
    }
}

namespace {

QImage filled(QColor colour, int size = 96)
{
    QImage image(size, size, QImage::Format_ARGB32);
    image.fill(colour);
    return image;
}

// A large dull area with a smaller vivid patch.
QImage dullWithAccent(QColor dull, QColor vivid, double vividShare)
{
    QImage image = filled(dull, 100);
    QPainter painter(&image);
    painter.fillRect(QRectF(0, 0, 100, 100.0 * vividShare), vivid);
    return image;
}

double hueDistance(double a, double b)
{
    const double d = std::fmod(std::abs(a - b), 360.0);
    return std::min(d, 360.0 - d);
}

}  // namespace

TEST_SUITE("theme.accent")
{
    TEST_CASE("the seed takes the hue of the dominant colourful area")
    {
        const QColor red(220, 30, 40);
        const auto analysis = analyseArtwork(filled(red), Extraction::Balanced);
        REQUIRE(analysis.seed.has_value());
        CHECK(hueDistance(analysis.seed->h, color::toOkLch(color::Srgb{220 / 255.0, 30 / 255.0, 40 / 255.0}).h) < 5.0);

        // A vivid minority beats a large grey area.
        const auto mixed =
            analyseArtwork(dullWithAccent(QColor(120, 120, 125), QColor(20, 120, 255), 0.25), Extraction::Balanced);
        REQUIRE(mixed.seed.has_value());
        CHECK(hueDistance(mixed.seed->h, color::toOkLch(color::Srgb{20 / 255.0, 120 / 255.0, 1.0}).h) < 10.0);
        CHECK_FALSE(mixed.swatches.empty());
    }

    TEST_CASE("pathological artwork: white, black, grayscale give no seed; pastel and neon stay usable")
    {
        CHECK_FALSE(analyseArtwork(filled(Qt::white), Extraction::Vivid).seed.has_value());
        CHECK_FALSE(analyseArtwork(filled(Qt::black), Extraction::Vivid).seed.has_value());
        QImage gradient(64, 64, QImage::Format_RGB32);
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                gradient.setPixel(x, y, qRgb(x * 4, x * 4, x * 4));
            }
        }
        CHECK_FALSE(analyseArtwork(gradient, Extraction::Vivid).seed.has_value());
        CHECK_FALSE(analyseArtwork(QImage(), Extraction::Vivid).seed.has_value());

        for (const QColor colour : {QColor(255, 200, 220), QColor(57, 255, 20), QColor(255, 0, 255)}) {
            CAPTURE(colour.name().toStdString());
            const auto analysis = analyseArtwork(filled(colour), Extraction::Vivid);
            REQUIRE(analysis.seed.has_value());
            CHECK(analysis.seed->L >= 0.5);
            CHECK(analysis.seed->L <= 0.78);
            // Whatever the art, the generated palette meets AA in both schemes.
            for (const Scheme scheme : {Scheme::Dark, Scheme::Light}) {
                const Palette palette = generatePalette({.seed = *analysis.seed, .scheme = scheme});
                for (const auto& result : checkContrast(palette, scheme, defaultTones(scheme))) {
                    CHECK(result.passes());
                }
            }
        }
    }

    TEST_CASE("dark and black-and-white artwork gets no invented colour")
    {
        // Near-black with compression-like chroma noise: no hue to speak of.
        QImage noisy(64, 64, QImage::Format_RGB32);
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const int n = (x * 7 + y * 13) % 9;
                noisy.setPixel(x, y, qRgb(14 + n, 10 + (n % 4), 12 + ((n * 3) % 7)));
            }
        }
        const auto dark = analyseArtwork(noisy, Extraction::Vivid);
        CHECK_FALSE(dark.seed.has_value());
        CHECK(dark.achromatic);

        // A sepia-toned black-and-white photo: a faint warm tint over every grey.
        QImage sepia(64, 64, QImage::Format_RGB32);
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const int v = 20 + (x * 3);
                sepia.setPixel(x, y, qRgb(std::min(255, v + 6), std::min(255, v + 3), v));
            }
        }
        CHECK(analyseArtwork(sepia, Extraction::Vivid).achromatic);
        CHECK_FALSE(analyseArtwork(sepia, Extraction::Vivid).seed.has_value());

        // Black with a few red specks: too little of the artwork to stand for it.
        QImage specks = filled(Qt::black, 100);
        {
            QPainter painter(&specks);
            painter.fillRect(QRect(10, 10, 5, 5), QColor(230, 20, 30));
        }
        CHECK(analyseArtwork(specks, Extraction::Vivid).achromatic);

        // A black cover with a clear red title keeps its red.
        QImage title = filled(Qt::black, 100);
        {
            QPainter painter(&title);
            painter.fillRect(QRect(10, 40, 80, 8), QColor(230, 20, 30));
        }
        const auto titled = analyseArtwork(title, Extraction::Vivid);
        REQUIRE(titled.seed.has_value());
        CHECK(hueDistance(titled.seed->h, color::toOkLch(color::Srgb{230 / 255.0, 20 / 255.0, 30 / 255.0}).h) < 10.0);

        // White and black artwork keep their swatches, even without a seed.
        CHECK_FALSE(analyseArtwork(filled(Qt::white), Extraction::Vivid).swatches.empty());
    }

    TEST_CASE("a muted cover gives a muted accent, not a boosted one")
    {
        const QColor dusty(140, 100, 70);
        const double source = color::toOkLch(color::Srgb{140 / 255.0, 100 / 255.0, 70 / 255.0}).C;
        const auto analysis = analyseArtwork(filled(dusty), Extraction::Vivid);
        REQUIRE(analysis.seed.has_value());
        CHECK(analysis.seed->C <= (source * 1.6) + 1e-3);
        CHECK(analysis.seed->C < 0.13);  // the old fixed floor of the vivid mode
        // A colourful cover is still lifted to a vivid accent.
        CHECK(analyseArtwork(filled(QColor(30, 90, 200)), Extraction::Vivid).seed->C >= 0.13);
    }

    TEST_CASE("extraction modes order the chroma; results are deterministic")
    {
        const QImage art = dullWithAccent(QColor(40, 30, 60), QColor(255, 90, 20), 0.5);
        const auto vivid = analyseArtwork(art, Extraction::Vivid);
        const auto balanced = analyseArtwork(art, Extraction::Balanced);
        const auto soft = analyseArtwork(art, Extraction::Soft);
        REQUIRE(vivid.seed.has_value());
        REQUIRE(balanced.seed.has_value());
        REQUIRE(soft.seed.has_value());
        CHECK(vivid.seed->C >= balanced.seed->C);
        CHECK(balanced.seed->C >= soft.seed->C);
        const auto again = analyseArtwork(art, Extraction::Vivid);
        CHECK(again.seed->h == vivid.seed->h);
        CHECK(again.seed->C == vivid.seed->C);
        CHECK(parseExtraction(QStringLiteral("soft")) == Extraction::Soft);
        CHECK(parseExtraction(QStringLiteral("bogus")) == Extraction::Vivid);
    }

    TEST_CASE("the cover backdrop stays inside each scheme's legible lightness range")
    {
        const QImage art = dullWithAccent(Qt::white, Qt::black, 0.5);
        for (const Scheme scheme : {Scheme::Dark, Scheme::Light}) {
            const QImage backdrop = blurredBackdrop(art, scheme, 64);
            REQUIRE(backdrop.size() == QSize(64, 64));
            const SchemeTones& tones = defaultTones(scheme);
            for (const QPoint p : {QPoint(0, 0), QPoint(32, 32), QPoint(63, 63)}) {
                const QColor c = backdrop.pixelColor(p);
                const double lightness =
                    color::toOkLab(color::Srgb{static_cast<double>(c.redF()), static_cast<double>(c.greenF()),
                                               static_cast<double>(c.blueF())})
                        .L;
                CHECK(lightness >= tones.backdropMinL - 0.02);
                CHECK(lightness <= tones.backdropMaxL + 0.02);
            }
        }
    }

    TEST_CASE("AccentEngine delivers seeds asynchronously, caches per artwork and clears")
    {
        settings::SettingsStore store(settings::Schema::builtin());
        AccentEngine engine(store);
        QSignalSpy seeds(&engine, &AccentEngine::seedChanged);
        engine.setArtwork(QStringLiteral("a"), filled(QColor(30, 200, 90)));
        REQUIRE(test::waitUntil([&] { return seeds.count() == 1; }));
        CHECK(engine.hasArtwork());
        CHECK_FALSE(engine.backdrop(Scheme::Dark).isNull());

        engine.setArtwork(QStringLiteral("b"), filled(QColor(200, 40, 200)));
        REQUIRE(test::waitUntil([&] { return seeds.count() == 2; }));
        engine.setArtwork(QStringLiteral("a"), filled(QColor(30, 200, 90)));
        CHECK(seeds.count() == 3);  // cache hit: delivered without a worker round trip

        REQUIRE(store.setUserValue(keys::AccentExtraction, QStringLiteral("soft")).has_value());
        REQUIRE(test::waitUntil([&] { return seeds.count() == 4; }));

        // Black and white artwork: a neutral accent instead of the previous track's.
        engine.setArtwork(QStringLiteral("grey"), filled(QColor(90, 90, 90)));
        REQUIRE(test::waitUntil([&] { return seeds.count() == 5; }));
        const auto neutral = seeds.last().first().value<std::optional<color::OkLch>>();
        REQUIRE(neutral.has_value());
        CHECK(neutral->C == 0.0);

        engine.clearArtwork();
        CHECK(seeds.count() == 6);
        CHECK_FALSE(seeds.last().first().value<std::optional<color::OkLch>>().has_value());
        CHECK_FALSE(engine.hasArtwork());
    }
}
