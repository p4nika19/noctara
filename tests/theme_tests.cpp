#include "TestUtil.hpp"

#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"
#include "theme/ColorScience.hpp"
#include "theme/Palette.hpp"
#include "theme/Theme.hpp"

#include <QSignalSpy>

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
        CHECK(backdrop.L >= 0.15);                          // deep, but not black
        CHECK(backdrop.C >= 0.02);                          // visibly tinted
        CHECK((backdrop.h > 250.0 && backdrop.h < 320.0));  // blue-violet
        CHECK(light.surfaceBase.alpha > dark.surfaceBase.alpha);
        CHECK(color::relativeLuminance(light.textPrimary.rgb) < color::relativeLuminance(dark.textPrimary.rgb));
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
        theme.setBlurAvailable(false);
        CHECK(theme.glass().style == QStringLiteral("solid"));
        CHECK_FALSE(theme.glass().translucent);
        theme.setBlurAvailable(true);
        theme.setGovernorStyleCap(QStringLiteral("solid"));
        CHECK(theme.glass().style == QStringLiteral("solid"));
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
}
