#include "TestUtil.hpp"

#include "i18n/LanguageManager.hpp"
#include "settings/ConfigFile.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"
#include "ui/IconProvider.hpp"
#include "ui/SettingsController.hpp"
#include "ui/SettingsModel.hpp"

#include <QColor>
#include <QTemporaryDir>

#include <set>

using namespace noctara;

TEST_SUITE("ui.settings")
{
    TEST_CASE("generated pages cover every schema entry exactly once (no orphan or missing rows)")
    {
        QTemporaryDir dir;
        settings::SettingsStore store(settings::Schema::builtin());
        settings::ConfigFile config(store, dir.filePath(QStringLiteral("config.toml")), {});
        i18n::LanguageManager languages(store);
        ui::SettingsController controller(store, config, languages);

        std::set<QString> seen;
        for (const QVariant& entry : controller.categories()) {
            const QString id = entry.toMap().value(QStringLiteral("id")).toString();
            CHECK_FALSE(entry.toMap().value(QStringLiteral("label")).toString().isEmpty());
            if (id == u"about") {
                continue;
            }
            ui::SettingsModel* page = controller.page(id);
            REQUIRE(page != nullptr);
            CHECK(page == controller.page(id));  // lazily created once
            for (int row = 0; row < page->rowCount(); ++row) {
                const QString key = page->data(page->index(row), ui::SettingsModel::KeyRole).toString();
                CHECK(seen.insert(key).second);
                CHECK_FALSE(page->data(page->index(row), ui::SettingsModel::LabelRole).toString().isEmpty());
            }
        }
        for (const settings::Definition& def : store.schema().definitions()) {
            if (!def.configOnly()) {
                CHECK(seen.contains(QString::fromLatin1(def.key.data(), static_cast<qsizetype>(def.key.size()))));
            }
        }
    }

    TEST_CASE("edits, validation messages, search and restart tracking")
    {
        QTemporaryDir dir;
        settings::SettingsStore store(settings::Schema::builtin());
        settings::ConfigFile config(store, dir.filePath(QStringLiteral("config.toml")), {});
        i18n::LanguageManager languages(store);
        ui::SettingsController controller(store, config, languages);

        CHECK(controller.setValue(QStringLiteral("appearance.text.ui_scale"), 125.0).isEmpty());
        CHECK(store.intValue(settings::keys::UiScale) == 125);
        CHECK_FALSE(controller.setValue(QStringLiteral("appearance.text.ui_scale"), 900.0).isEmpty());

        CHECK_FALSE(controller.restartPending());
        CHECK(controller.setValue(QStringLiteral("performance.gpu.acceleration"), QStringLiteral("off")).isEmpty());
        CHECK(controller.restartPending());
        controller.reset(QStringLiteral("performance.gpu.acceleration"));
        CHECK_FALSE(controller.restartPending());

        controller.setSearchText(QStringLiteral("blur"));
        CHECK(controller.searchResults()->rowCount() >= 1);
        controller.setSearchText(QStringLiteral("zzzz-nothing"));
        CHECK(controller.searchResults()->rowCount() == 0);
    }

    TEST_CASE("dependent rows are hidden until their condition holds")
    {
        QTemporaryDir dir;
        settings::SettingsStore store(settings::Schema::builtin());
        settings::ConfigFile config(store, dir.filePath(QStringLiteral("config.toml")), {});
        i18n::LanguageManager languages(store);
        ui::SettingsController controller(store, config, languages);
        ui::SettingsModel* page = controller.page(QStringLiteral("appearance"));
        REQUIRE(page != nullptr);

        const auto shown = [page](const char* key) {
            for (int row = 0; row < page->rowCount(); ++row) {
                if (page->data(page->index(row), ui::SettingsModel::KeyRole).toString() == QLatin1StringView(key)) {
                    return page->data(page->index(row), ui::SettingsModel::ShownRole).toBool();
                }
            }
            FAIL("missing row");
            return false;
        };
        CHECK_FALSE(shown("appearance.accent.color"));
        REQUIRE(store.setUserValue(settings::keys::AccentAdaptive, false).has_value());
        CHECK(shown("appearance.accent.color"));
        CHECK_FALSE(shown("appearance.text.heading_font"));  // advanced
        controller.setShowAdvanced(true);
        CHECK(shown("appearance.text.heading_font"));
    }
}

TEST_SUITE("ui.presets")
{
    TEST_CASE("presets apply ordinary settings and are detected from the current values")
    {
        QTemporaryDir dir;
        settings::SettingsStore store(settings::Schema::builtin());
        settings::ConfigFile config(store, dir.filePath(QStringLiteral("config.toml")), {});
        i18n::LanguageManager languages(store);
        ui::SettingsController controller(store, config, languages);

        CHECK(controller.presets().size() == 2);
        CHECK(controller.currentPreset().isEmpty());  // the default theme mode follows the system

        CHECK(controller.setValue(QStringLiteral("appearance.glass.buttons"), QStringLiteral("liquid")).isEmpty());
        controller.applyPreset(QStringLiteral("frost"));
        CHECK(controller.currentPreset() == QStringLiteral("frost"));
        CHECK(store.stringValue(settings::keys::ThemeMode) == QStringLiteral("light"));
        // Presets change colours only: other appearance settings are left alone.
        CHECK(store.stringValue(settings::keys::ButtonMaterial) == QStringLiteral("liquid"));

        controller.applyPreset(QStringLiteral("midnight"));
        CHECK(controller.currentPreset() == QStringLiteral("midnight"));
        CHECK(controller.setValue(QStringLiteral("appearance.theme.mode"), QStringLiteral("system")).isEmpty());
        CHECK(controller.currentPreset().isEmpty());

        const QStringList drawerKeys{QStringLiteral("appearance.glow.intensity"),
                                     QStringLiteral("appearance.glass.buttons")};
        ui::SettingsModel* subset = controller.subset(drawerKeys);
        REQUIRE(subset != nullptr);
        CHECK(subset->rowCount() == 2);
        CHECK(subset == controller.subset(drawerKeys));
        controller.resetKeys(drawerKeys);
        CHECK(store.stringValue(settings::keys::ButtonMaterial) == QStringLiteral("frosted"));
    }
}

TEST_SUITE("ui.icons")
{
    TEST_CASE("SVG tinting replaces currentColor and stroke width")
    {
        const QByteArray svg = R"(<svg stroke="currentColor" stroke-width="2"><path d="M0 0"/></svg>)";
        const QByteArray tinted = ui::IconProvider::tintSvg(svg, QColor(QStringLiteral("#336699")), 1.5);
        CHECK(tinted.contains("stroke=\"#336699\""));
        CHECK(tinted.contains("stroke-width=\"1.5\""));
        CHECK_FALSE(tinted.contains("currentColor"));
        const QByteArray translucent = ui::IconProvider::tintSvg(svg, QColor(0x33, 0x66, 0x99, 128), 2.0);
        CHECK(translucent.contains("opacity="));
    }
}
