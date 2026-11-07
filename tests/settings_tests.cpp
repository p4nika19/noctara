#include "TestUtil.hpp"

#include "core/FileIo.hpp"
#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "settings/ConfigFile.hpp"
#include "settings/Keys.hpp"
#include "settings/Migrations.hpp"
#include "settings/Overrides.hpp"
#include "settings/SchemaDoc.hpp"
#include "settings/SettingsStore.hpp"
#include "settings/TomlCodec.hpp"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <unistd.h>

using namespace noctara;
using namespace noctara::settings;

namespace {

const Schema& schema()
{
    return Schema::builtin();
}

std::size_t idx(std::string_view key)
{
    const auto index = schema().indexOf(key);
    REQUIRE(index.has_value());
    return *index;
}

ParsedConfig parse(std::string_view text)
{
    return parseConfig(text, QStringLiteral("test.toml"), schema());
}

std::optional<QVariant> parsedValue(const ParsedConfig& parsed, std::string_view key)
{
    for (const auto& [index, value] : parsed.values) {
        if (index == idx(key)) {
            return value;
        }
    }
    return std::nullopt;
}

bool writeText(const QString& path, const QByteArray& content)
{
    return writeFileAtomic(path, content).has_value();
}

}  // namespace

TEST_SUITE("settings.schema")
{
    TEST_CASE("builtin schema is consistent and secret-free")
    {
        static const QRegularExpression secretLike(QStringLiteral("password|passwd|token|secret|cookie|credential"));
        REQUIRE(schema().size() > 10);
        for (const Definition& def : schema().definitions()) {
            CHECK(validate(def, def.defaultValue).has_value());
            CHECK_FALSE(
                QString::fromLatin1(def.key.data(), static_cast<qsizetype>(def.key.size())).contains(secretLike));
            CHECK(schema().section(def.sectionId()) != nullptr);
        }
    }

    TEST_CASE("environment variable names")
    {
        CHECK(Schema::environmentVariable(*schema().find(keys::ThemeMode)) ==
              QStringLiteral("NOCTARA_APPEARANCE_THEME_MODE"));
    }

    TEST_CASE("validation rejects wrong types, ranges and enum values")
    {
        const Definition& theme = *schema().find(keys::ThemeMode);
        CHECK(validate(theme, QStringLiteral("dark")).has_value());
        CHECK(test::errorOf(validate(theme, QStringLiteral("purple"))) == ErrorCode::ConfigUnknownEnumValue);
        CHECK(test::errorOf(validate(theme, true)) == ErrorCode::ConfigTypeMismatch);

        const Definition& zoom = *schema().find(keys::UiScale);
        CHECK(validate(zoom, QVariant::fromValue(qint64{150})).has_value());
        CHECK(test::errorOf(validate(zoom, QVariant::fromValue(qint64{500}))) == ErrorCode::ConfigOutOfRange);
        CHECK(test::errorOf(validate(zoom, QVariant::fromValue(150.5))) == ErrorCode::ConfigTypeMismatch);
        CHECK(validate(zoom, QVariant::fromValue(150.0))->typeId() == QMetaType::LongLong);

        const Definition& color = *schema().find(keys::AccentColor);
        CHECK(validate(color, QStringLiteral("#AABBCC"))->toString() == QStringLiteral("#aabbcc"));
        CHECK_FALSE(validate(color, QStringLiteral("red")).has_value());
    }

    TEST_CASE("a rejected DoH URL does not carry its credentials into the error")
    {
        const auto rejected = validate(*schema().find(keys::DohUrl), QStringLiteral("https://u:pw@x.example/q"));
        REQUIRE_FALSE(rejected.has_value());
        const std::string text = test::describe(rejected);
        CHECK(text.find("pw") == std::string::npos);
        CHECK(text.find("x.example/q") != std::string::npos);
    }

    TEST_CASE("textual values from env/CLI")
    {
        const Definition& adaptive = *schema().find(keys::AccentAdaptive);
        CHECK(parseText(adaptive, u"off")->toBool() == false);
        CHECK(parseText(adaptive, u" YES ")->toBool() == true);
        CHECK_FALSE(parseText(adaptive, u"maybe").has_value());
        CHECK(parseText(*schema().find(keys::AnimationSpeed), u"1.5")->toDouble() == doctest::Approx(1.5));
    }
}

TEST_SUITE("settings.store")
{
    TEST_CASE("layer precedence: default < system < user < env < cli")
    {
        SettingsStore store(schema());
        const auto mode = idx(keys::ThemeMode);
        CHECK(store.stringValue(keys::ThemeMode) == QStringLiteral("system"));
        store.replaceLayer(Layer::System, {{mode, QStringLiteral("light")}});
        CHECK(store.stringValue(keys::ThemeMode) == QStringLiteral("light"));
        store.replaceLayer(Layer::User, {{mode, QStringLiteral("dark")}});
        CHECK(store.stringValue(keys::ThemeMode) == QStringLiteral("dark"));
        store.replaceLayer(Layer::Environment, {{mode, QStringLiteral("system")}});
        CHECK(store.effectiveLayer(mode) == Layer::Environment);
        store.replaceLayer(Layer::CommandLine, {{mode, QStringLiteral("light")}});
        CHECK(store.stringValue(keys::ThemeMode) == QStringLiteral("light"));
        CHECK(store.effectiveLayer(mode) == Layer::CommandLine);
    }

    TEST_CASE("UI edits are validated, refused when overridden, and default-equal values are not stored")
    {
        SettingsStore store(schema());
        QSignalSpy changed(&store, &SettingsStore::valueChanged);
        QSignalSpy edited(&store, &SettingsStore::userLayerEdited);

        REQUIRE(store.setUserValue(keys::ThemeMode, QStringLiteral("dark")).has_value());
        CHECK(changed.count() == 1);
        CHECK(edited.count() == 1);
        CHECK(store.layerValue(Layer::User, idx(keys::ThemeMode)).has_value());

        REQUIRE(store.setUserValue(keys::ThemeMode, QStringLiteral("system")).has_value());
        CHECK_FALSE(store.layerValue(Layer::User, idx(keys::ThemeMode)).has_value());

        CHECK(test::errorOf(store.setUserValue(keys::ThemeMode, QStringLiteral("nope"))) ==
              ErrorCode::ConfigUnknownEnumValue);
        CHECK(test::errorOf(store.setUserValue("no.such.key", true)) == ErrorCode::ConfigUnknownKey);

        store.replaceLayer(Layer::Environment, {{idx(keys::ThemeMode), QStringLiteral("light")}});
        CHECK(test::errorOf(store.setUserValue(keys::ThemeMode, QStringLiteral("dark"))) ==
              ErrorCode::ConfigOverridden);
    }

    TEST_CASE("reset section and all")
    {
        SettingsStore store(schema());
        REQUIRE(store.setUserValue(keys::GlassGrain, 0.6).has_value());
        REQUIRE(store.setUserValue(keys::ThemeMode, QStringLiteral("dark")).has_value());
        store.resetSection("appearance.glass");
        CHECK(store.realValue(keys::GlassGrain) == doctest::Approx(0.0));
        CHECK(store.stringValue(keys::ThemeMode) == QStringLiteral("dark"));
        store.resetAll();
        CHECK(store.layerSnapshot(Layer::User).empty());
    }
}

TEST_SUITE("settings.toml")
{
    TEST_CASE("parses known keys with validation and source positions")
    {
        const auto parsed = parse("config_version = 1\n"
                                  "[appearance.theme]\nmode = \"dark\"\n"
                                  "[appearance.text]\nui_scale = 400\n"
                                  "[appearance.glass]\nblur_strength = 1\n");
        REQUIRE(parsed.syntaxOk);
        CHECK(parsedValue(parsed, keys::ThemeMode)->toString() == QStringLiteral("dark"));
        CHECK(parsedValue(parsed, keys::GlassBlurStrength)->toDouble() == doctest::Approx(1.0));
        CHECK_FALSE(parsedValue(parsed, keys::UiScale).has_value());
        REQUIRE(parsed.issues.size() == 1);
        CHECK(parsed.issues.front().code == ErrorCode::ConfigOutOfRange);
        CHECK(parsed.issues.front().line == 5);
        CHECK(parsed.issues.front().key == QStringLiteral("appearance.text.ui_scale"));
    }

    TEST_CASE("syntax errors yield no values")
    {
        const auto parsed = parse("[appearance.theme\nmode = \"dark\"\n");
        CHECK_FALSE(parsed.syntaxOk);
        CHECK(parsed.values.empty());
        REQUIRE(parsed.issues.size() == 1);
        CHECK(parsed.issues.front().code == ErrorCode::ConfigSyntaxError);
    }

    TEST_CASE("unknown keys survive a rewrite and documented defaults stay commented")
    {
        const auto parsed = parse("[appearance.theme]\nmode = \"light\"\nfuture_option = 3\n"
                                  "[plugins.extra]\nname = \"кириллица 🎵\"\nlist = [1, 2]\n");
        REQUIRE(parsed.syntaxOk);
        CHECK(std::ranges::count_if(parsed.issues, [](const ConfigIssue& issue) {
                  return issue.code == ErrorCode::ConfigUnknownKey;
              }) == 3);

        const QByteArray rewritten = serializeConfig(schema(), parsed.values, parsed.unknownToml);
        CHECK(rewritten.contains("# blur_strength = 0.6"));
        CHECK(rewritten.contains("mode = \"light\""));

        const auto reparsed =
            parse(std::string_view(rewritten.constData(), static_cast<std::size_t>(rewritten.size())));
        REQUIRE(reparsed.syntaxOk);
        CHECK(parsedValue(reparsed, keys::ThemeMode)->toString() == QStringLiteral("light"));
        CHECK(reparsed.unknownToml == parsed.unknownToml);
        CHECK(QString::fromUtf8(rewritten).contains(QStringLiteral("кириллица 🎵")));
    }

    TEST_CASE("unknown root keys and arrays of tables in a settings table keep their place in a rewrite")
    {
        const auto parsed = parse("client_id = \"misplaced\"\nfoo = 1\n"
                                  "[[appearance.glass.layers]]\nx = 1\n[[appearance.glass.layers]]\nx = 2\n"
                                  "[soundcloud]\nclient_id = \"abcdefghijklmnopqrstuvwxyz012345\"\n");
        REQUIRE(parsed.syntaxOk);
        REQUIRE_FALSE(parsed.unknownToml.empty());

        const QByteArray rewritten = serializeConfig(schema(), parsed.values, parsed.unknownToml);
        CAPTURE(rewritten.toStdString());
        const auto reparsed =
            parse(std::string_view(rewritten.constData(), static_cast<std::size_t>(rewritten.size())));
        REQUIRE(reparsed.syntaxOk);
        CHECK(reparsed.unknownToml == parsed.unknownToml);
        CHECK(parsedValue(reparsed, keys::ClientId)->toString() == QStringLiteral("abcdefghijklmnopqrstuvwxyz012345"));
    }

    TEST_CASE("a value where a settings table belongs is reported and the rewrite stays valid TOML")
    {
        for (const std::string text : {"[cache]\nimages = 512\n", "[general]\nformatting = \"24h\"\n",
                                       "network = 1\n[cache]\nimages = { future = 1 }\n"}) {
            CAPTURE(text);
            const auto parsed = parse(text);
            REQUIRE(parsed.syntaxOk);
            CHECK(std::ranges::any_of(parsed.issues, [](const ConfigIssue& issue) {
                return issue.severity == ConfigIssue::Severity::Error || issue.code == ErrorCode::ConfigUnknownKey;
            }));
            const QByteArray rewritten = serializeConfig(schema(), parsed.values, parsed.unknownToml);
            const auto reparsed =
                parse(std::string_view(rewritten.constData(), static_cast<std::size_t>(rewritten.size())));
            CHECK(reparsed.syntaxOk);
        }

        // Keys of a settings table written inline still apply; unknown ones are kept in that table.
        const auto inlined =
            parse("[cache]\nimages = { future = 1 }\n[network]\ndns = { fallback_to_system = false }\n");
        REQUIRE(inlined.syntaxOk);
        CHECK(parsedValue(inlined, keys::DnsFallbackToSystem)->toBool() == false);
        const QByteArray rewritten = serializeConfig(schema(), inlined.values, inlined.unknownToml);
        const auto reparsed =
            parse(std::string_view(rewritten.constData(), static_cast<std::size_t>(rewritten.size())));
        REQUIRE(reparsed.syntaxOk);
        CHECK(reparsed.unknownToml == inlined.unknownToml);
        CHECK(parsedValue(reparsed, keys::DnsFallbackToSystem)->toBool() == false);
    }

    TEST_CASE("every schema value round-trips through the serializer")
    {
        ValueMap values;
        values.emplace_back(idx(keys::AccentColor), QStringLiteral("#123abc"));
        values.emplace_back(idx(keys::AnimationSpeed), 0.25);
        values.emplace_back(idx(keys::UiScale), QVariant::fromValue(qint64{125}));
        values.emplace_back(idx(keys::HeadingFont), QStringLiteral("Quote \" back\\slash\ttab"));
        const QByteArray text = serializeConfig(schema(), values, {});
        const auto parsed = parse(std::string_view(text.constData(), static_cast<std::size_t>(text.size())));
        REQUIRE(parsed.syntaxOk);
        CHECK(parsed.issues.empty());
        CHECK(parsed.values == values);
    }

    TEST_CASE("v1 battery_saver migrates to the three-way energy_saving setting")
    {
        for (const char* text : {"config_version = 1\n[performance.power]\nbattery_saver = false\n",
                                 "[performance.power]\nbattery_saver = false\n"}) {
            const auto parsed = parse(text);
            REQUIRE(parsed.syntaxOk);
            CHECK(parsedValue(parsed, keys::EnergySaving)->toString() == QStringLiteral("off"));
            CHECK(parsed.issues.empty());
            CHECK(parsed.unknownToml.empty());
        }
        const auto enabled = parse("config_version = 1\n[performance.power]\nbattery_saver = true\n");
        CHECK(parsedValue(enabled, keys::EnergySaving)->toString() == QStringLiteral("auto"));
    }

    TEST_CASE("v2 cover_art backdrop flag migrates to the backdrop style")
    {
        const auto off = parse("config_version = 2\n[appearance.backdrop]\ncover_art = false\n");
        REQUIRE(off.syntaxOk);
        CHECK(parsedValue(off, keys::BackdropStyle)->toString() == QStringLiteral("dark"));
        CHECK(off.issues.empty());
        const auto on = parse("config_version = 2\n[appearance.backdrop]\ncover_art = true\n");
        CHECK_FALSE(parsedValue(on, keys::BackdropStyle).has_value());  // default (aurora)
        CHECK(on.unknownToml.empty());
    }

    TEST_CASE("v6 Liquid Glass style splits into the Dual Kawase blur and the Liquid surface")
    {
        const auto liquid = parse("config_version = 6\n[appearance.glass]\nstyle = \"liquid\"\n");
        REQUIRE(liquid.syntaxOk);
        CHECK(liquid.issues.empty());
        CHECK(parsedValue(liquid, keys::GlassStyle)->toString() == QStringLiteral("kawase"));
        CHECK(parsedValue(liquid, keys::GlassSurface)->toString() == QStringLiteral("liquid"));
        const auto gaussian = parse("config_version = 6\n[appearance.glass]\nstyle = \"gaussian\"\n");
        CHECK(parsedValue(gaussian, keys::GlassStyle)->toString() == QStringLiteral("gaussian"));
        CHECK_FALSE(parsedValue(gaussian, keys::GlassSurface).has_value());  // default (frosted)
    }

    TEST_CASE("glass tint and grain: v4 relative values become absolute again, v3 values pass unchanged")
    {
        const auto v4 = parse("config_version = 4\n[appearance.glass]\nopacity = 0.3\ngrain = -0.35\n");
        REQUIRE(v4.syntaxOk);
        CHECK(v4.issues.empty());
        CHECK(parsedValue(v4, keys::GlassOpacity)->toDouble() == doctest::Approx(1.0));  // 0.8, then +20 % (v6)
        CHECK(parsedValue(v4, keys::GlassGrain)->toDouble() == doctest::Approx(0.0));
        const auto v3 = parse("config_version = 3\n[appearance.glass]\nopacity = 0.8\ngrain = 0.2\n");
        CHECK(parsedValue(v3, keys::GlassOpacity)->toDouble() == doctest::Approx(1.0));
        const auto v5 = parse("config_version = 5\n[appearance.glass]\nopacity = 0.3\n");
        CHECK(parsedValue(v5, keys::GlassOpacity)->toDouble() == doctest::Approx(0.5));
        CHECK(parsedValue(v3, keys::GlassGrain)->toDouble() == doctest::Approx(0.2));
    }

    TEST_CASE("migrations move keys between versions")
    {
        auto parsed = toml::parse("[old]\nname = 5\n");
        REQUIRE(parsed);
        toml::table root = std::move(parsed).table();
        const Migration migrations[] = {
            {1, [](toml::table& table) { moveKey(table, "old.name", "fresh.section.name"); }}};
        CHECK(migrate(root, 1, 2, migrations) == 2);
        CHECK(root.at_path("fresh.section.name").value<int64_t>() == 5);
        CHECK_FALSE(root.at_path("old.name"));
    }
}

TEST_SUITE("settings.overrides")
{
    TEST_CASE("environment and command line")
    {
        QProcessEnvironment env;
        env.insert(QStringLiteral("NOCTARA_APPEARANCE_THEME_MODE"), QStringLiteral("light"));
        env.insert(QStringLiteral("NOCTARA_APPEARANCE_TEXT_UI_SCALE"), QStringLiteral("9000"));
        const auto fromEnv = collectEnvironment(schema(), env);
        CHECK(fromEnv.values.size() == 1);
        CHECK(fromEnv.issues.size() == 1);

        using Kind = CommandLineOverride::Kind;
        const auto fromCli = collectCommandLine(schema(), {{Kind::Set, QStringLiteral("appearance.glass.grain=0.1")},
                                                           {Kind::Set, QStringLiteral("bogus=1")},
                                                           {Kind::Alias, QStringLiteral("gpu=off")},
                                                           {Kind::Alias, QStringLiteral("nope=1")}});
        CHECK(fromCli.values.size() == 2);
        CHECK(fromCli.issues.size() == 2);
    }

    TEST_CASE("the later of an alias and a --set for one key wins")
    {
        using Kind = CommandLineOverride::Kind;
        const auto mode = [](const OverrideSet& set) {
            REQUIRE(set.values.size() == 1);
            CHECK(set.values.front().first == idx(keys::ThemeMode));
            return set.values.front().second.toString();
        };
        CHECK(mode(collectCommandLine(schema(), {{Kind::Alias, QStringLiteral("theme=dark")},
                                                 {Kind::Set, QStringLiteral("appearance.theme.mode=light")}})) ==
              QStringLiteral("light"));
        CHECK(mode(collectCommandLine(schema(), {{Kind::Set, QStringLiteral("appearance.theme.mode=light")},
                                                 {Kind::Alias, QStringLiteral("theme=dark")}})) ==
              QStringLiteral("dark"));
    }
}

TEST_SUITE("settings.file")
{
    TEST_CASE("first run writes a template; edits are saved atomically; external edits hot-reload")
    {
        QTemporaryDir dir;
        REQUIRE(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("config/config.toml"));

        SettingsStore store(schema());
        ConfigFile file(store, path, {});
        file.setSaveDelay(std::chrono::milliseconds(1));
        CHECK(file.load().isEmpty());
        file.flush();
        REQUIRE(QFile::exists(path));
        CHECK(QFileInfo(path).permissions() ==
              (QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser | QFileDevice::WriteUser));

        REQUIRE(store.setUserValue(keys::ThemeMode, QStringLiteral("dark")).has_value());
        REQUIRE(test::waitUntil([&] { return readWholeFile(path)->contains("\nmode = \"dark\""); }));

        file.setHotReloadEnabled(true);
        QSignalSpy reloaded(&file, &ConfigFile::reloaded);
        QSignalSpy issues(&file, &ConfigFile::reloadIssues);

        // A valid external edit is applied live.
        REQUIRE(writeText(
            path, "config_version = 6\n[appearance.theme]\nmode = \"light\"\n[appearance.glass]\ngrain = 0.5\n"));
        REQUIRE(test::waitUntil([&] { return store.stringValue(keys::ThemeMode) == QStringLiteral("light"); }));
        CHECK(store.realValue(keys::GlassGrain) == doctest::Approx(0.5));

        // An invalid value keeps the last good one and reports the problem.
        REQUIRE(writeText(path,
                          "config_version = 6\n[appearance.theme]\nmode = \"light\"\n[appearance.glass]\ngrain = 7\n"));
        REQUIRE(test::waitUntil([&] { return issues.count() > 0; }));
        CHECK(store.realValue(keys::GlassGrain) == doctest::Approx(0.5));

        // A syntax error keeps everything.
        issues.clear();
        REQUIRE(writeText(path, "[appearance.theme\n"));
        REQUIRE(test::waitUntil([&] { return issues.count() > 0; }));
        CHECK(store.stringValue(keys::ThemeMode) == QStringLiteral("light"));

        // Saving over a hand edit that does not parse keeps it as a backup, as at startup.
        REQUIRE(store.setUserValue(keys::GlassGrain, 0.3).has_value());
        file.flush();
        const QString backup = path + QStringLiteral(".bak");
        REQUIRE(QFile::exists(backup));
        CHECK(readWholeFile(backup)->contains("[appearance.theme\n"));
    }

    TEST_CASE("a broken file is backed up before being overwritten, and reset/import/export work")
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("config.toml"));
        REQUIRE(writeText(path, "this is = = not toml"));

        SettingsStore store(schema());
        ConfigFile file(store, path, {});
        file.setSaveDelay(std::chrono::milliseconds(1));
        const auto issues = file.load();
        REQUIRE(issues.size() == 1);
        CHECK(issues.front().code == ErrorCode::ConfigSyntaxError);

        REQUIRE(store.setUserValue(keys::GlassGrain, 0.6).has_value());
        file.flush();
        CHECK(readWholeFile(path + QStringLiteral(".bak"))->contains("not toml"));

        const QString exported = dir.filePath(QStringLiteral("profile.toml"));
        REQUIRE(file.exportTo(exported).has_value());
        REQUIRE(file.resetToDefaults().has_value());
        CHECK(store.realValue(keys::GlassGrain) == doctest::Approx(0.0));
        REQUIRE(file.importFrom(exported).has_value());
        CHECK(store.realValue(keys::GlassGrain) == doctest::Approx(0.6));
    }

    TEST_CASE("a failed save is reported and written by the next flush")
    {
        if (::geteuid() == 0) {
            return;  // root ignores the directory permissions this case relies on
        }
        QTemporaryDir dir;
        REQUIRE(dir.isValid());
        const QString configDir = dir.filePath(QStringLiteral("config"));
        const QString path = configDir + QStringLiteral("/config.toml");

        SettingsStore store(schema());
        ConfigFile file(store, path, {});
        file.setSaveDelay(std::chrono::milliseconds(1));
        CHECK(file.load().isEmpty());
        file.flush();
        REQUIRE(QFile::exists(path));

        const auto writable = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
        const auto restore = qScopeGuard([&] { QFile::setPermissions(configDir, writable); });
        REQUIRE(QFile::setPermissions(configDir, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        QSignalSpy failed(&file, &ConfigFile::saveFailed);
        REQUIRE(store.setUserValue(keys::GlassGrain, 0.4).has_value());
        file.flush();
        REQUIRE(test::waitUntil([&] { return failed.count() == 1; }));
        CHECK_FALSE(readWholeFile(path)->contains("\ngrain = 0.4"));

        REQUIRE(QFile::setPermissions(configDir, writable));
        file.flush();
        CHECK(readWholeFile(path)->contains("\ngrain = 0.4"));
    }

    TEST_CASE("an unreadable config file is not overwritten when it cannot be backed up")
    {
        if (::geteuid() == 0) {
            return;  // root reads the file regardless of its permissions
        }
        QTemporaryDir dir;
        REQUIRE(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("config.toml"));
        const QString backup = path + QStringLiteral(".bak");
        const QByteArray original = "config_version = 6\n[appearance.glass]\ngrain = 0.7\n";
        REQUIRE(writeText(path, original));
        REQUIRE(writeText(backup, "older backup"));
        const auto readable = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
        const auto restore = qScopeGuard([&] { QFile::setPermissions(path, readable); });
        REQUIRE(QFile::setPermissions(path, {}));

        SettingsStore store(schema());
        ConfigFile file(store, path, {});
        file.setSaveDelay(std::chrono::milliseconds(1));
        CHECK_FALSE(file.load().isEmpty());
        QSignalSpy failed(&file, &ConfigFile::saveFailed);
        REQUIRE(store.setUserValue(keys::GlassGrain, 0.2).has_value());
        file.flush();
        REQUIRE(test::waitUntil([&] { return failed.count() == 1; }));

        REQUIRE(QFile::setPermissions(path, readable));
        CHECK(readWholeFile(path).value_or(QByteArray()) == original);
        CHECK(readWholeFile(backup).value_or(QByteArray()) == "older backup");

        // Once the file is readable again, the next save backs it up and writes the edit.
        file.flush();
        CHECK(readWholeFile(backup).value_or(QByteArray()) == original);
        CHECK(readWholeFile(path).value_or(QByteArray()).contains("\ngrain = 0.2"));
    }

    TEST_CASE("importing an unreadable profile reports a read error, not a syntax error")
    {
        if (::geteuid() == 0) {
            return;  // root reads the file regardless of its permissions
        }
        QTemporaryDir dir;
        REQUIRE(dir.isValid());
        const QString profile = dir.filePath(QStringLiteral("profile.toml"));
        REQUIRE(writeText(profile, "config_version = 6\n"));
        REQUIRE(QFile::setPermissions(profile, {}));

        SettingsStore store(schema());
        ConfigFile file(store, dir.filePath(QStringLiteral("config.toml")), {});
        const auto imported = file.importFrom(profile);
        REQUIRE_FALSE(imported.has_value());
        CHECK(test::errorOf(imported) == ErrorCode::IoError);

        const auto broken = dir.filePath(QStringLiteral("broken.toml"));
        REQUIRE(writeText(broken, "this is = = not toml"));
        CHECK(test::errorOf(file.importFrom(broken)) == ErrorCode::ConfigSyntaxError);
    }

    TEST_CASE("values dropped from an imported profile are logged")
    {
        QTemporaryDir dir;
        REQUIRE(dir.isValid());
        const QString profile = dir.filePath(QStringLiteral("profile.toml"));
        REQUIRE(writeText(profile, "config_version = 6\n[appearance.glass]\ngrain = 7\n"));

        static QStringList warnings;  // importFrom runs on this thread
        warnings.clear();
        const QtMessageHandler previous =
            qInstallMessageHandler([](QtMsgType type, const QMessageLogContext&, const QString& message) {
                if (type == QtWarningMsg) {
                    warnings.append(message);
                }
            });
        const auto restore = qScopeGuard([&] { qInstallMessageHandler(previous); });

        SettingsStore store(schema());
        ConfigFile file(store, dir.filePath(QStringLiteral("config.toml")), {});
        const auto imported = file.importFrom(profile);
        REQUIRE(imported.has_value());
        CHECK_FALSE(imported->isEmpty());
        CHECK(std::ranges::any_of(warnings, [](const QString& line) { return line.contains(u"grain"); }));
    }

    TEST_CASE("the system config file is the first XDG_CONFIG_DIRS entry that has one")
    {
        QTemporaryDir dir;
        REQUIRE(dir.isValid());
        const QString app = QCoreApplication::applicationName();
        // Plasma prepends ~/.config/kdedefaults, Ubuntu /etc/xdg/xdg-ubuntu: neither holds Noctara's file.
        const QString kdeDefaults = dir.filePath(QStringLiteral("kdedefaults"));
        const QString etc = dir.filePath(QStringLiteral("etc"));
        const QString systemFile = etc + u'/' + app + QStringLiteral("/config.toml");
        REQUIRE(QDir().mkpath(kdeDefaults + u'/' + app));
        REQUIRE(QDir().mkpath(etc + u'/' + app));
        REQUIRE(writeText(systemFile, "config_version = 6\n"));

        const QByteArray previousHome = qgetenv("XDG_CONFIG_HOME");
        const QByteArray previousDirs = qgetenv("XDG_CONFIG_DIRS");
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(dir.filePath(QStringLiteral("home"))));
        qputenv("XDG_CONFIG_DIRS", QFile::encodeName(kdeDefaults + u':' + etc));
        const QString found = AppPaths::standard().systemConfigFile;
        qputenv("XDG_CONFIG_DIRS", QFile::encodeName(kdeDefaults + u':' + dir.filePath(QStringLiteral("none"))));
        const QString fallback = AppPaths::standard().systemConfigFile;
        const auto restore = [](const char* name, const QByteArray& previous) {
            if (previous.isNull()) {
                qunsetenv(name);
            } else {
                qputenv(name, previous);
            }
        };
        restore("XDG_CONFIG_HOME", previousHome);
        restore("XDG_CONFIG_DIRS", previousDirs);

        CHECK(found == systemFile);
        // Without any system file the first system directory is still named (documented location).
        CHECK(fallback == kdeDefaults + u'/' + app + QStringLiteral("/config.toml"));
    }
}

TEST_SUITE("settings.report")
{
    TEST_CASE("print-config shows sources and redacts URL credentials")
    {
        CHECK(log::redactUrlCredentials(u"https://user:pa55@dns.example/dns-query") ==
              QStringLiteral("https://<redacted>@dns.example/dns-query"));
        CHECK(log::redactUrlCredentials(u"https://dns.example/q@x") == QStringLiteral("https://dns.example/q@x"));

        SettingsStore store(schema());
        store.replaceLayer(Layer::CommandLine, {{idx(keys::ThemeMode), QStringLiteral("dark")}});
        const QString report = effectiveConfigReport(store);
        CHECK(report.contains(QStringLiteral("mode = \"dark\"  # command line")));
    }

    TEST_CASE("generated docs mention every key")
    {
        const QString markdown = schemaMarkdown(schema());
        for (const Definition& def : schema().definitions()) {
            const QString leaf =
                QString::fromLatin1(def.leafName().data(), static_cast<qsizetype>(def.leafName().size()));
            CHECK(markdown.contains(u'`' + leaf + u'`'));
        }
    }
}
