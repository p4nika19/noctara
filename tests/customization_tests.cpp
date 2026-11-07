// Slice (h): user theme files (parsing, validation, application, the library with hot reload and import), the
// keyboard shortcut settings (validation, conflicts, the recorder's conversion), the layout settings and window
// state, and the trending-by-genre system playlists that replaced SoundCloud's charts.
#include "TestUtil.hpp"
#include "api/Endpoints.hpp"
#include "api/Parse.hpp"

#include "core/FileIo.hpp"
#include "i18n/LanguageManager.hpp"
#include "settings/ConfigFile.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"
#include "theme/Palette.hpp"
#include "theme/Theme.hpp"
#include "theme/ThemeLibrary.hpp"
#include "theme/UserTheme.hpp"
#include "ui/KeyboardController.hpp"
#include "ui/LayoutController.hpp"
#include "ui/SettingsController.hpp"

#include <QFile>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>

using namespace noctara;
using namespace noctara::theme;
namespace keys = settings::keys;

namespace {

constexpr auto kAurora = R"(
[theme]
name = "Aurora"
description = "Cold greens"
accent = "#3ddc97"

[colors.dark]
backdrop = "#06110d"
textPrimary = "#f0fff8"

[colors.light]
accent = "#0b6e4f"

[radius]
lg = 34
button = 12

[space]
rowHeight = 64

[fonts]
heading = "Bundled Sans That Does Not Exist"

[motion]
normal = 260

[glass.dark]
refraction = 22
lightAngle = 120
)";

std::vector<ThemeIssue> issuesOf(const char* toml)
{
    std::vector<ThemeIssue> issues;
    (void)parseUserTheme(QByteArray(toml), QStringLiteral("t"), issues);
    return issues;
}

FontFamilies bundled()
{
    return {QStringLiteral("Manrope"), QStringLiteral("Inter"), QStringLiteral("JetBrains Mono")};
}

void write(const QString& path, const QByteArray& content)
{
    REQUIRE(writeFileAtomic(path, content, 0600, Durability::Atomic));
}

}  // namespace

TEST_SUITE("theme.user")
{
    TEST_CASE("a theme file overrides colors per scheme, shapes, fonts, motion and glass")
    {
        std::vector<ThemeIssue> issues;
        const Result<UserTheme> parsed = parseUserTheme(QByteArray(kAurora), QStringLiteral("aurora"), issues);
        REQUIRE(parsed);
        CHECK(issues.empty());
        CHECK(parsed->name == u"Aurora");
        CHECK(parsed->description == u"Cold greens");
        REQUIRE(parsed->accent.has_value());
        CHECK(parsed->colors[0].size() == 2);
        CHECK(parsed->colors[1].count("accent") == 1);
        CHECK(parsed->radius.at("lg") == doctest::Approx(34));
        CHECK(parsed->space.at("rowHeight") == doctest::Approx(64));
        CHECK(parsed->motion.at("normal") == 260);
        CHECK(parsed->glass[0].at("refraction") == doctest::Approx(22));

        Palette palette{};
        applyColors(*parsed, Scheme::Dark, palette);
        CHECK(color::toArgb32(palette.backdrop) == 0xff06110dU);
        RadiusTokens radius;
        applyRadius(*parsed, radius);
        CHECK(radius.lg == doctest::Approx(34));
        CHECK(radius.md == doctest::Approx(RadiusTokens{}.md));  // untouched
    }

    TEST_CASE("unknown tokens and invalid values are reported and skipped; a syntax error rejects the file")
    {
        const auto issues = issuesOf(R"(
[colors.dark]
notARole = "#ffffff"
accent = "red"
[radius]
lg = -4
[glass.dusk]
refraction = 3
[sounds]
click = "x.wav"
)");
        // Tables are read in key order, so look the issues up by key.
        const auto issue = [&](const char16_t* key) {
            const auto it = std::ranges::find(issues, QStringView(key), &ThemeIssue::key);
            REQUIRE(it != issues.end());
            return *it;
        };
        REQUIRE(issues.size() == 5);
        CHECK(issue(u"colors.dark.notARole").kind == ThemeIssue::Kind::UnknownToken);
        CHECK(issue(u"colors.dark.notARole").line == 3);
        CHECK(issue(u"colors.dark.accent").kind == ThemeIssue::Kind::InvalidValue);
        CHECK(issue(u"radius.lg").kind == ThemeIssue::Kind::InvalidValue);
        CHECK(issue(u"glass.dusk").kind == ThemeIssue::Kind::UnknownToken);
        CHECK(issue(u"sounds").kind == ThemeIssue::Kind::UnknownToken);

        std::vector<ThemeIssue> broken;
        const Result<UserTheme> parsed =
            parseUserTheme(QByteArray("[colors.dark\naccent = 1"), QStringLiteral("x"), broken);
        REQUIRE_FALSE(parsed);
        CHECK(test::errorOf(parsed) == ErrorCode::ConfigSyntaxError);
        REQUIRE(broken.size() == 1);
        CHECK(broken[0].kind == ThemeIssue::Kind::Syntax);
    }

    TEST_CASE("the theme applies to the Theme singleton; unreadable overrides are corrected when contrast is enforced")
    {
        settings::SettingsStore store(settings::Schema::builtin());
        REQUIRE(store.setUserValue(keys::ThemeMode, QStringLiteral("dark")));
        REQUIRE(store.setUserValue(keys::AnimationSpeed, 0.0));
        Theme theme(store, bundled());
        const qreal rowBefore = theme.space().rowHeight;
        std::vector<ThemeIssue> issues;
        auto userTheme = std::make_shared<const UserTheme>(*parseUserTheme(R"(
[colors.dark]
backdrop = "#06110d"
textPrimary = "#151515"
[space]
rowHeight = 80
)",
                                                                           QStringLiteral("dim"), issues));
        theme.setUserTheme(userTheme);
        CHECK(theme.space().rowHeight == doctest::Approx(80 * theme.scale()));
        CHECK(theme.space().rowHeight != doctest::Approx(rowBefore));
        CHECK(theme.colors().backdrop.name() == QStringLiteral("#06110d"));
        // Near-black text on dark glass: lifted until it reads.
        CHECK(theme.contrastOk());
        CHECK(theme.colors().textPrimary.lightnessF() > 0.5F);

        REQUIRE(store.setUserValue(keys::EnforceContrast, false));
        CHECK_FALSE(theme.contrastOk());
        CHECK(theme.colors().textPrimary.name() == QStringLiteral("#151515"));

        theme.setUserTheme(nullptr);
        CHECK(theme.space().rowHeight == doctest::Approx(rowBefore));
    }

    TEST_CASE("a light accent container from a theme gets readable on-container text when contrast is enforced")
    {
        settings::SettingsStore store(settings::Schema::builtin());
        REQUIRE(store.setUserValue(keys::ThemeMode, QStringLiteral("dark")));
        REQUIRE(store.setUserValue(keys::AnimationSpeed, 0.0));
        Theme theme(store, bundled());
        std::vector<ThemeIssue> issues;
        theme.setUserTheme(std::make_shared<const UserTheme>(
            *parseUserTheme("[colors.dark]\naccentContainer = \"#e0e0e0\"\n", QStringLiteral("pale"), issues)));
        CHECK(theme.colors().accentContainer.name() == QStringLiteral("#e0e0e0"));
        CHECK(theme.contrastOk());
    }

    TEST_CASE("a theme's accent seeds the palette when the cover art does not decide")
    {
        settings::SettingsStore store(settings::Schema::builtin());
        REQUIRE(store.setUserValue(keys::AccentAdaptive, false));
        REQUIRE(store.setUserValue(keys::AnimationSpeed, 0.0));
        Theme theme(store, bundled());
        const QColor before = theme.colors().accent;
        std::vector<ThemeIssue> issues;
        theme.setUserTheme(std::make_shared<const UserTheme>(
            *parseUserTheme("[theme]\naccent = \"#e04a2a\"\n", QStringLiteral("ember"), issues)));
        const QColor after = theme.colors().accent;
        CHECK(after != before);
        CHECK(std::abs(after.hueF() - QColor(0xe0, 0x4a, 0x2a).hueF()) < 0.05F);
    }
}

TEST_SUITE("theme.examples")
{
    TEST_CASE("the example themes in docs/themes parse without a single issue")
    {
        const std::vector<ThemeEntry> themes = scanThemes(QStringLiteral(NOCTARA_SOURCE_DIR "/docs/themes"));
        REQUIRE(themes.size() == 3);
        for (const ThemeEntry& entry : themes) {
            CAPTURE(entry.id);
            CHECK(entry.theme != nullptr);
            CHECK(entry.issues.empty());
            CHECK_FALSE(entry.description.isEmpty());
        }
    }
}

TEST_SUITE("theme.library")
{
    TEST_CASE("themes are found in the folder, the selected one is active, edits reload it, imports land there")
    {
        QTemporaryDir dir;
        const QString folder = dir.filePath(QStringLiteral("themes"));
        REQUIRE(ensureDirectory(folder));
        write(folder + QStringLiteral("/aurora.toml"), QByteArray(kAurora));
        write(folder + QStringLiteral("/broken.toml"), "[colors\n");
        write(folder + QStringLiteral("/notes.txt"), "not a theme");

        settings::SettingsStore store(settings::Schema::builtin());
        ThemeLibrary library(store, folder);
        REQUIRE(test::waitUntil([&] { return library.scanned(); }));
        REQUIRE(library.themes().size() == 2);
        CHECK(library.themes()[0].id == u"aurora");
        CHECK(library.themes()[0].theme != nullptr);
        CHECK(library.themes()[1].theme == nullptr);  // syntax error: listed, unusable
        CHECK(library.active() == nullptr);

        QSignalSpy active(&library, &ThemeLibrary::activeChanged);
        REQUIRE(store.setUserValue(keys::CustomTheme, QStringLiteral("aurora")));
        REQUIRE(library.active() != nullptr);
        CHECK(library.active()->name == u"Aurora");

        // Hot reload: the edited file replaces the active theme.
        write(folder + QStringLiteral("/aurora.toml"), "[theme]\nname = \"Aurora 2\"\n");
        REQUIRE(test::waitUntil([&] { return library.active() && library.active()->name == u"Aurora 2"; }, 5000));

        // Import copies a valid file under a free name and refuses broken ones.
        const QString outside = dir.filePath(QStringLiteral("aurora.toml"));
        write(outside, QByteArray(kAurora));
        QObject context;
        std::optional<Result<QString>> imported;
        library.importFile(outside, &context, [&](Result<QString> id) { imported = std::move(id); });
        REQUIRE(test::waitUntil([&] { return imported.has_value(); }));
        REQUIRE(*imported);
        CHECK(**imported == u"aurora-2");
        CHECK(QFile::exists(folder + QStringLiteral("/aurora-2.toml")));
        imported.reset();
        library.importFile(folder + QStringLiteral("/broken.toml"), &context,
                           [&](Result<QString> id) { imported = std::move(id); });
        REQUIRE(test::waitUntil([&] { return imported.has_value(); }));
        CHECK(test::errorOf(*imported) == ErrorCode::ConfigSyntaxError);

        // A selected theme that disappears falls back to the built-in look.
        REQUIRE(QFile::remove(folder + QStringLiteral("/aurora.toml")));
        library.rescan();
        REQUIRE(test::waitUntil([&] { return library.active() == nullptr; }, 5000));
        CHECK(library.selectedEntry() == nullptr);
    }
}

TEST_SUITE("ui.keyboard")
{
    TEST_CASE("shortcut and navigation settings are validated")
    {
        settings::SettingsStore store(settings::Schema::builtin());
        const std::string_view next = "keyboard.shortcuts.next";
        for (const char* ok : {"Ctrl+Shift+Q", "Space", "Ctrl+,", "Alt+Left", "Meta+F5", ""}) {
            CAPTURE(ok);
            CHECK(store.setUserValue(next, QString::fromLatin1(ok)));
        }
        for (const char* bad : {"Ctrl+", "Ctrl+Shift", "Ctrl+K, Ctrl+L"}) {
            CAPTURE(bad);
            CHECK_FALSE(store.setUserValue(next, QString::fromLatin1(bad)));
        }
        CHECK(store.setUserValue(keys::NavItems, QStringList{QStringLiteral("charts"), QStringLiteral("home")}));
        CHECK_FALSE(store.setUserValue(keys::NavItems, QStringList{QStringLiteral("home"), QStringLiteral("home")}));
        CHECK_FALSE(store.setUserValue(keys::NavItems, QStringList{QStringLiteral("radio")}));
    }

    TEST_CASE("the controller publishes sequences, flags conflicts on both rows and converts key presses")
    {
        QTemporaryDir dir;
        settings::SettingsStore store(settings::Schema::builtin());
        settings::ConfigFile config(store, dir.filePath(QStringLiteral("config.toml")), {});
        i18n::LanguageManager languages(store);
        ui::SettingsController settingsUi(store, config, languages);
        ui::KeyboardController keyboard(store, languages, settingsUi);

        CHECK(keyboard.sequences().value(QStringLiteral("play_pause")).toString() == u"Space");
        CHECK(settingsUi.rowStatus(QStringLiteral("keyboard.shortcuts.next")).isEmpty());
        QSignalSpy changed(&keyboard, &ui::KeyboardController::changed);
        REQUIRE(store.setUserValue("keyboard.shortcuts.next", QStringLiteral("ctrl+k")));  // same as Search (Ctrl+K)
        CHECK_FALSE(changed.isEmpty());
        CHECK_FALSE(settingsUi.rowStatus(QStringLiteral("keyboard.shortcuts.next")).isEmpty());
        CHECK_FALSE(settingsUi.rowStatus(QStringLiteral("keyboard.shortcuts.search")).isEmpty());
        REQUIRE(store.setUserValue("keyboard.shortcuts.next", QStringLiteral("Ctrl+Right")));
        CHECK(settingsUi.rowStatus(QStringLiteral("keyboard.shortcuts.search")).isEmpty());

        CHECK(ui::KeyboardController::fromKeyEvent(Qt::Key_K, Qt::ControlModifier) == u"Ctrl+K");
        CHECK(ui::KeyboardController::fromKeyEvent(Qt::Key_Q, Qt::ControlModifier | Qt::ShiftModifier) ==
              u"Ctrl+Shift+Q");
        CHECK(ui::KeyboardController::fromKeyEvent(Qt::Key_Shift, Qt::ShiftModifier).isEmpty());
        const QVariantList sheet = keyboard.cheatSheet();
        CHECK(sheet.size() >= 15);
        CHECK_FALSE(sheet.first().toMap().value(QStringLiteral("label")).toString().isEmpty());
    }
}

TEST_SUITE("ui.layout")
{
    TEST_CASE("window state round-trips and rejects nonsense; the start page can be the last one")
    {
        ui::WindowState state;
        state.geometry = QRect(10, 20, 1300, 900);
        state.maximized = true;
        state.page = QStringLiteral("charts");
        const ui::WindowState back = ui::WindowState::fromJson(state.toJson());
        CHECK(back.geometry == state.geometry);
        CHECK(back.maximized);
        CHECK(back.page == u"charts");
        CHECK_FALSE(ui::WindowState::fromJson(R"({"version":1,"width":3,"height":2})").geometry.isValid());
        CHECK_FALSE(ui::WindowState::fromJson("garbage").geometry.isValid());

        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("window.json"));
        write(path, state.toJson());
        settings::SettingsStore store(settings::Schema::builtin());
        ui::LayoutController layout(store, path);
        CHECK(layout.startPage() == u"home");
        REQUIRE(store.setUserValue(keys::StartPage, QStringLiteral("last")));
        CHECK(layout.startPage() == u"charts");
        QSignalSpy changed(&layout, &ui::LayoutController::changed);
        layout.setView(QStringLiteral("charts"), QStringLiteral("grid"));
        CHECK(layout.views().value(QStringLiteral("charts")).toString() == u"grid");
        CHECK_FALSE(changed.isEmpty());
    }

    TEST_CASE("a window maximized before any normal size was saved opens maximized again")
    {
        // A first run that quits maximized knows no normal geometry, so it saves 0x0 with maximized = true.
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("window.json"));
        write(path, R"({"version":1,"x":0,"y":0,"width":0,"height":0,"maximized":true})");
        settings::SettingsStore store(settings::Schema::builtin());
        const ui::LayoutController layout(store, path);
        QQuickWindow window;
        layout.restore(window);
        CHECK((window.windowStates() & Qt::WindowMaximized) != 0);
    }
}

TEST_SUITE("api.trending")
{
    TEST_CASE("trending-by-genre system playlists parse into one page of tracks (known only by id)")
    {
        const Result<QByteArray> json =
            readWholeFile(QStringLiteral(NOCTARA_SOURCE_DIR "/tests/fixtures/api/system-playlist.json"));
        REQUIRE(json);
        const Result<api::Page<api::Entry>> page = api::parse::playlistTracks(*json);
        REQUIRE_MESSAGE(page, test::describe(page));
        REQUIRE(page->items.size() == 3);
        const auto* first = std::get_if<api::Track>(&page->items[0].item);
        REQUIRE(first != nullptr);
        CHECK(first->id == 2422820481);
        CHECK(first->stub);
        CHECK(page->next.isEmpty());

        const auto endpoint = api::endpoints::trending(QStringLiteral("hip-hop/../../me"));
        // Only letters, digits and '-' reach the path.
        CHECK(endpoint.request.url.path() ==
              u"/system-playlists/soundcloud:system-playlists:trending-by-genre:hip-hopme");
    }
}
