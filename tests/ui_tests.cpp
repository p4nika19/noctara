#include "TestUtil.hpp"

#include "gfx/FrameMonitor.hpp"
#include "gfx/GraphicsBootstrap.hpp"
#include "gfx/PowerMonitor.hpp"
#include "i18n/LanguageManager.hpp"
#include "media/PlaybackQueue.hpp"
#include "media/QueueModel.hpp"
#include "models/CollectionModel.hpp"
#include "models/Details.hpp"
#include "settings/ConfigFile.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"
#include "theme/Theme.hpp"
#include "ui/EffectsController.hpp"
#include "ui/IconProvider.hpp"
#include "ui/SettingsController.hpp"
#include "ui/SettingsModel.hpp"
#include "ui/WaveformItem.hpp"

#include <QAbstractItemModelTester>
#include <QColor>
#include <QJSEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <set>

using namespace noctara;

TEST_SUITE("ui.effects")
{
    TEST_CASE("the quality governor keeps measuring at Solid and brings the blur back when there is headroom")
    {
        QTemporaryDir dir;
        settings::SettingsStore store(settings::Schema::builtin());
        settings::ConfigFile config(store, dir.filePath(QStringLiteral("config.toml")), {});
        REQUIRE(store.setUserValue(settings::keys::EnergySaving, QStringLiteral("off")));
        i18n::LanguageManager languages(store);
        ui::SettingsController settingsUi(store, config, languages);
        theme::Theme theme(store, {});
        gfx::PowerMonitor power;
        gfx::FrameMonitor frames;
        gfx::GraphicsDecision decision;
        decision.hardware = true;
        const gfx::GraphicsInfo graphics(decision);
        gfx::GovernorConfig governor;
        governor.upCooldownS = 0.0;  // the controller's clock is real time
        ui::EffectsController effects(store, theme, power, frames, graphics, languages, settingsUi, governor);
        REQUIRE(theme.glass().style == u"kawase");

        gfx::FrameWindowStats overloaded;
        overloaded.frames = 60;
        overloaded.gpuP90Ms = 20.0;
        for (int window = 0; window < 8; ++window) {
            Q_EMIT frames.measured(overloaded, 50.0);
        }
        REQUIRE(effects.tier() == u"solid");
        REQUIRE_FALSE(theme.glass().blurred);

        gfx::FrameWindowStats healthy = overloaded;
        healthy.gpuP90Ms = 2.0;
        for (int window = 0; window < 5; ++window) {
            Q_EMIT frames.measured(healthy, 60.0);
        }
        CHECK(effects.tier() == u"kawase");
        CHECK(theme.glass().style == u"kawase");
    }
}

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
    TEST_CASE("a row whose parent row is hidden stays hidden (chained conditions)")
    {
        QTemporaryDir dir;
        settings::SettingsStore store(settings::Schema::builtin());
        settings::ConfigFile config(store, dir.filePath(QStringLiteral("config.toml")), {});
        i18n::LanguageManager languages(store);
        ui::SettingsController controller(store, config, languages);
        ui::SettingsModel* page = controller.page(QStringLiteral("network"));
        REQUIRE(page != nullptr);
        const auto shown = [page](std::string_view key) {
            for (int row = 0; row < page->rowCount(); ++row) {
                if (page->data(page->index(row), ui::SettingsModel::KeyRole).toString() ==
                    QLatin1StringView(key.data(), static_cast<qsizetype>(key.size()))) {
                    return page->data(page->index(row), ui::SettingsModel::ShownRole).toBool();
                }
            }
            FAIL("missing row");
            return false;
        };
        // The server list depends on the preset, which only exists in custom DNS mode.
        REQUIRE(store.setUserValue(settings::keys::DnsServerPreset, QStringLiteral("custom")).has_value());
        CHECK_FALSE(shown(settings::keys::DnsServerPreset));
        CHECK_FALSE(shown(settings::keys::DnsServers));
        REQUIRE(store.setUserValue(settings::keys::DnsMode, QStringLiteral("custom")).has_value());
        CHECK(shown(settings::keys::DnsServerPreset));
        CHECK(shown(settings::keys::DnsServers));
        CHECK_FALSE(shown(settings::keys::DohUrl));
    }

    TEST_CASE("presets apply ordinary settings and are detected from the current values")
    {
        QTemporaryDir dir;
        settings::SettingsStore store(settings::Schema::builtin());
        settings::ConfigFile config(store, dir.filePath(QStringLiteral("config.toml")), {});
        i18n::LanguageManager languages(store);
        ui::SettingsController controller(store, config, languages);

        CHECK(controller.presets().size() == 3);
        CHECK(controller.currentPreset() == QStringLiteral("auto"));  // the default theme mode follows the system

        CHECK(controller.setValue(QStringLiteral("appearance.glass.buttons"), QStringLiteral("liquid")).isEmpty());
        controller.applyPreset(QStringLiteral("frost"));
        CHECK(controller.currentPreset() == QStringLiteral("frost"));
        CHECK(store.stringValue(settings::keys::ThemeMode) == QStringLiteral("light"));
        // Presets change colours only: other appearance settings are left alone.
        CHECK(store.stringValue(settings::keys::ButtonMaterial) == QStringLiteral("liquid"));

        controller.applyPreset(QStringLiteral("midnight"));
        CHECK(controller.currentPreset() == QStringLiteral("midnight"));
        CHECK(controller.setValue(QStringLiteral("appearance.theme.mode"), QStringLiteral("system")).isEmpty());
        CHECK(controller.currentPreset() == QStringLiteral("auto"));

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

namespace {

namespace stub {

using Factory = std::function<QJSValue(QJSEngine*)>;

std::map<QString, Factory>& factories()
{
    static std::map<QString, Factory> registry;
    return registry;
}

// Makes the Noctara singleton `name` whatever `factory` returns, in every engine created from now on. Each name
// is registered with QML once; later calls only swap the factory.
void singleton(const char* name, Factory factory)
{
    static std::set<QString> registered;
    const QString key = QString::fromLatin1(name);
    factories()[key] = std::move(factory);
    if (registered.insert(key).second) {
        qmlRegisterSingletonType("Noctara", 1, 0, name,
                                 [key](QQmlEngine*, QJSEngine* js) { return factories().at(key)(js); });
    }
}

void singleton(const char* name, const QString& source)
{
    singleton(name, [source](QJSEngine* js) { return js->evaluate(source); });
}

}  // namespace stub

// Solid squares in the requested colour, loaded asynchronously like the real icon provider.
class SolidIconProvider : public QQuickImageProvider {
public:
    SolidIconProvider()
        : QQuickImageProvider(QQuickImageProvider::Image, QQmlImageProviderBase::ForceAsynchronousImageLoading)
    {
    }

    QImage requestImage(const QString& id, QSize* size, const QSize& /*requestedSize*/) override
    {
        const QString colour = QUrl::fromPercentEncoding(id.section(u'=', 1, 1).section(u'&', 0, 0).toLatin1());
        QImage image(4, 4, QImage::Format_ARGB32_Premultiplied);
        image.fill(QColor::fromString(colour));
        if (size != nullptr) {
            *size = image.size();
        }
        return image;
    }
};

// Icon.qml with a stub Theme singleton (only the tokens it reads).
struct IconHarness {
    QQmlEngine engine;
    std::unique_ptr<QObject> icon;

    IconHarness()
    {
        stub::singleton("Theme", QStringLiteral("({targetColors: {iconPrimary: '#101010'}, space: {iconSize: 16}, "
                                                "iconStroke: 1.5, motion: {normal: 0}})"));
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): QQmlEngine takes ownership of image providers
        engine.addImageProvider(QStringLiteral("icon"), new SolidIconProvider);
        QQmlComponent component(&engine,
                                QUrl::fromLocalFile(QStringLiteral(NOCTARA_SOURCE_DIR "/src/qml/components/Icon.qml")));
        icon.reset(component.createWithInitialProperties({{QStringLiteral("name"), QStringLiteral("play")}}));
        REQUIRE_MESSAGE(icon != nullptr, component.errorString());
    }

    // The colour of the layer that is currently shown (the other one is faded out).
    [[nodiscard]] QColor shownColour() const
    {
        const int current = icon->property("current").toInt();
        int index = 0;
        for (QObject* child : icon->children()) {
            if (child->inherits("QQuickImage") && index++ == current) {
                const QString source = child->property("source").toUrl().toString();
                return QColor::fromString(
                    QUrl::fromPercentEncoding(source.section(u'=', 1, 1).section(u'&', 0, 0).toLatin1()));
            }
        }
        return {};
    }

    void setColour(const char* colour) const
    {
        icon->setProperty("color", QColor::fromString(QLatin1StringView(colour)));
    }
    [[nodiscard]] bool settlesOn(const char* colour) const
    {
        return test::waitUntil([&] { return shownColour() == QColor::fromString(QLatin1StringView(colour)); }, 2000);
    }
};

}  // namespace

TEST_SUITE("ui.icons")
{
    TEST_CASE("an icon always ends up showing its current colour")
    {
        IconHarness harness;
        REQUIRE(harness.settlesOn("#101010"));

        // Cold loads, then returns to colours both layers or the pixmap cache already hold.
        const std::array colours{"#ff0000", "#00ff00", "#ff0000", "#00ff00", "#0000ff", "#ff0000", "#0000ff"};
        for (const char* colour : colours) {
            harness.setColour(colour);
            CHECK_MESSAGE(harness.settlesOn(colour), colour, " shown: ", harness.shownColour().name());
        }
        // Rapid changes (selection moving across items, theme crossfade): only the last colour matters.
        for (int round = 0; round < 20; ++round) {
            for (const char* colour : colours) {
                harness.setColour(colour);
            }
            if (round % 3 == 0) {
                QCoreApplication::processEvents();
            }
            harness.setColour(colours.at(static_cast<std::size_t>(round) % colours.size()));
            CHECK(harness.settlesOn(colours.at(static_cast<std::size_t>(round) % colours.size())));
        }
    }
}

namespace {

// Every token the components read, with quiet values: no blur, flat surfaces, instant motion except `slow`.
// Main buttons are drawn "like other buttons" (not accent-filled).
constexpr auto kThemeStub = R"js(({
    scale: 1, dark: true, coverShape: 'rounded', iconStroke: 1.5,
    colors: {
        accent: '#3366ff', accentDeep: '#2244aa', accentLight: '#88aaff', accentText: '#6688ff', onAccent: '#ffffff',
        backdrop: '#000000', danger: '#ff4444', divider: '#333333', edge: '#444444', focusRing: '#6688ff',
        outline: '#555555', rimHighlight: '#ffffff', scrim: '#000000', selection: '#334488', shadow: '#000000',
        sheen: '#ffffff', skeletonBase: '#222222', skeletonHighlight: '#333333', solidBase: '#111111',
        solidOverlay: '#1a1a1a', solidRaised: '#161616', stateHover: '#20ffffff', statePressed: '#30ffffff',
        surfaceBase: '#111111', surfaceOverlay: '#1a1a1a', surfaceRaised: '#161616', textPrimary: '#eeeeee',
        textSecondary: '#bbbbbb', textTertiary: '#888888', warning: '#ffaa00'
    },
    targetColors: {
        accent: '#3366ff', onAccent: '#ffffff', iconPrimary: '#eeeeee', iconSecondary: '#bbbbbb', danger: '#ff4444',
        success: '#44ff44', warning: '#ffaa00'
    },
    space: {
        xxs: 2, xs: 4, sm: 8, md: 12, lg: 16, xl: 24, xxl: 32, xxxl: 48, hairline: 1, focusRingWidth: 2,
        controlHeight: 40, controlHeightSmall: 32, iconSize: 20, iconSizeSmall: 16, iconSizeLarge: 28,
        rowHeight: 40, navRailWidth: 72, navRailWidthExpanded: 220, nowPlayingHeight: 80
    },
    radius: { xs: 4, sm: 6, md: 10, lg: 16, button: 20 },
    font: {
        body: Qt.font({pixelSize: 14}), bodyStrong: Qt.font({pixelSize: 14, bold: true}),
        caption: Qt.font({pixelSize: 12}), display: Qt.font({pixelSize: 32}), eyebrow: Qt.font({pixelSize: 11}),
        headline: Qt.font({pixelSize: 24}), label: Qt.font({pixelSize: 14}), mono: Qt.font({pixelSize: 13}),
        monoSmall: Qt.font({pixelSize: 11}), numeric: Qt.font({pixelSize: 13}), subtitle: Qt.font({pixelSize: 16}),
        title: Qt.font({pixelSize: 20})
    },
    motion: { fast: 0, normal: 0, slow: 400, accent: 0, shimmer: 0, reduced: false, easingStandard: 7,
              easingEmphasized: 7 },
    glass: {
        buttons: 'plain', primary: 'material', primaryOnAccent: false, flat: true, surface: 'flat', style: 'solid',
        blurred: false, translucent: false, backdrop: false, backdropAnimated: false, grain: 0, glow: 0,
        glowControls: false, glowCover: false, glowProgress: false, blurDownsample: 1, blurIterations: 0,
        blurOffset: 0, blurSigma: 0, brightnessLift: 0, saturationLift: 0, dispersion: 0, fresnel: 0, lightAngle: 0,
        refraction: 0, rimDarkening: 0, specular: 0, thickness: 0
    }
}))js";

constexpr auto kPlayerStub = R"js(({
    artworkUrl: 'https://example.com/cover.jpg', title: 'Title', artist: 'Artist', artistId: 0, context: '',
    duration: 0, position: 0, playing: false, canGoNext: false, shuffle: false, repeat: 0, volume: 1, muted: false,
    waveform: [], cachedRanges: [], errorCode: 0, queue: null
}))js";

void installStubs(stub::Factory player)
{
    static const bool waveform = [] {
        qmlRegisterType<ui::WaveformItem>("Noctara", 1, 0, "WaveformItem");
        return true;
    }();
    (void)waveform;
    stub::singleton("Theme", QString::fromUtf8(kThemeStub));
    stub::singleton("Graphics", QStringLiteral("({hardware: false, ndcYUp: true})"));
    stub::singleton("Format", QStringLiteral("({revision: 0, duration: ms => '0:00', error: code => ''})"));
    stub::singleton("Layout", QStringLiteral("({fullPlayerQueue: false, fullPlayerArt: 'large', "
                                             "fullPlayerWaveform: false, playerSize: 'normal', playerTime: true, "
                                             "playerWaveform: true, playerVolume: true, playerQueueButton: true})"));
    stub::singleton("Artwork", QStringLiteral("({hasArtwork: false, revision: 0})"));
    if (player) {
        stub::singleton("Player", std::move(player));
    } else {
        stub::singleton("Player", QString::fromUtf8(kPlayerStub));
    }
}

// A shown Window written in `qml`, resolved as if it lived next to the components (so they are found by name),
// with the stub singletons above. The window exposes the items a test looks at as properties.
struct WindowHarness {
    QQmlEngine engine;
    std::unique_ptr<QQuickWindow> window;

    explicit WindowHarness(const QByteArray& qml, QQmlImageProviderBase* cover = nullptr, stub::Factory player = {})
    {
        installStubs(std::move(player));
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): QQmlEngine takes ownership of image providers
        engine.addImageProvider(QStringLiteral("icon"), new SolidIconProvider);
        if (cover != nullptr) {
            engine.addImageProvider(QStringLiteral("cover"), cover);
        }
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport Noctara\n" + qml,
                          QUrl::fromLocalFile(QStringLiteral(NOCTARA_SOURCE_DIR "/src/qml/components/Harness.qml")));
        QObject* created = component.create();
        window.reset(qobject_cast<QQuickWindow*>(created));
        REQUIRE_MESSAGE(window != nullptr, component.errorString());
        REQUIRE(test::waitUntil([this] { return window->isExposed(); }));
    }

    [[nodiscard]] QObject* object(const char* name) const { return window->property(name).value<QObject*>(); }
    [[nodiscard]] QQuickItem* item(const char* name) const { return qobject_cast<QQuickItem*>(object(name)); }
};

// Counts image://cover requests (decode + mask jobs) and answers each with a small grey square.
class CountingCoverProvider : public QQuickImageProvider {
public:
    explicit CountingCoverProvider(std::atomic<int>& requests)
        : QQuickImageProvider(QQuickImageProvider::Image, QQmlImageProviderBase::ForceAsynchronousImageLoading)
        , m_requests(requests)
    {
    }

    QImage requestImage(const QString& /*id*/, QSize* size, const QSize& /*requestedSize*/) override
    {
        ++m_requests;
        QImage image(8, 8, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::gray);
        if (size != nullptr) {
            *size = image.size();
        }
        return image;
    }

private:
    std::atomic<int>& m_requests;
};

std::vector<media::QueueItem> queueItems(int count)
{
    std::vector<media::QueueItem> items;
    for (int i = 0; i < count; ++i) {
        media::QueueItem item;
        item.track.id = i + 1;
        item.track.title = QStringLiteral("Track %1").arg(i);
        items.push_back(std::move(item));
    }
    return items;
}

}  // namespace

TEST_SUITE("ui.components")
{
    TEST_CASE("a primary button's icon is accent-coloured when Main buttons are not accent-filled")
    {
        const WindowHarness harness(R"(Window {
            width: 300; height: 100; visible: true
            property Item button: button
            Button { id: button; variant: "primary"; iconName: "play"; text: "Play" }
        })");
        const auto* row = harness.item("button")->property("contentItem").value<QQuickItem*>();
        REQUIRE(row != nullptr);
        REQUIRE_FALSE(row->childItems().isEmpty());
        // The label is accentText here, the icon must not be onAccent (white on the button material).
        CHECK(row->childItems().at(0)->property("color").value<QColor>() == QColor(QStringLiteral("#3366ff")));
    }

    TEST_CASE("a dropdown keeps following currentId after a disabled entry was activated")
    {
        const WindowHarness harness(R"(Window {
            id: window
            width: 300; height: 100; visible: true
            property Item combo: combo
            property int picks: 0
            ComboBox {
                id: combo
                model: [{id: "a", label: "A"}, {id: "b", label: "B", enabled: false}, {id: "c", label: "C"}]
                currentId: "a"
                onPicked: window.picks++
            }
        })");
        QQuickItem* combo = harness.item("combo");
        REQUIRE(combo->property("currentIndex").toInt() == 0);
        harness.window->requestActivate();
        combo->forceActiveFocus();
        REQUIRE(test::waitUntil([&] { return combo->hasActiveFocus(); }));
        QTest::keyClick(harness.window.get(), Qt::Key_Down);  // activates "b", which is refused
        CHECK(harness.window->property("picks").toInt() == 0);
        CHECK(combo->property("currentIndex").toInt() == 0);

        combo->setProperty("currentId", QStringLiteral("c"));  // e.g. a section reset or a preset
        CHECK(combo->property("currentIndex").toInt() == 2);
    }

    TEST_CASE("a context menu opened near the bottom stays inside the window")
    {
        const WindowHarness harness(R"(Window {
            width: 400; height: 400; visible: true
            property QtObject menu: menu
            ContextMenu { id: menu }
        })");
        QObject* menu = harness.object("menu");
        QVariantList actions;
        for (int i = 0; i < 10; ++i) {
            actions.append(QVariantMap{{QStringLiteral("text"), QStringLiteral("Action %1").arg(i)}});
        }
        QVariant item = QVariant::fromValue<QObject*>(harness.window->contentItem());
        REQUIRE(QMetaObject::invokeMethod(menu, "show", Q_ARG(QVariant, QVariant(actions)), Q_ARG(QVariant, item),
                                          Q_ARG(QVariant, QVariant(QPointF(10, 390)))));
        REQUIRE(test::waitUntil([&] { return menu->property("height").toReal() > 300; }));
        CHECK(menu->property("y").toReal() + menu->property("height").toReal() <= 400);
        CHECK(menu->property("y").toReal() >= 0);
    }

    TEST_CASE("Up in a context menu with nothing highlighted highlights the last entry")
    {
        const WindowHarness harness(R"(Window {
            width: 400; height: 400; visible: true
            property QtObject menu: menu
            ContextMenu { id: menu }
        })");
        QObject* menu = harness.object("menu");
        QVariantList actions;
        for (int i = 0; i < 3; ++i) {
            actions.append(QVariantMap{{QStringLiteral("text"), QStringLiteral("Action %1").arg(i)}});
        }
        QVariant item = QVariant::fromValue<QObject*>(harness.window->contentItem());
        REQUIRE(QMetaObject::invokeMethod(menu, "show", Q_ARG(QVariant, QVariant(actions)), Q_ARG(QVariant, item),
                                          Q_ARG(QVariant, QVariant(QPointF(10, 10)))));
        REQUIRE(menu->property("highlighted").toInt() == -1);
        REQUIRE(QMetaObject::invokeMethod(menu, "step", Q_ARG(QVariant, QVariant(-1))));
        CHECK(menu->property("highlighted").toInt() == 2);
    }

    TEST_CASE("a context menu is as wide as its own entries, not as the widest earlier menu")
    {
        const WindowHarness harness(R"(Window {
            width: 800; height: 400; visible: true
            property QtObject menu: menu
            ContextMenu { id: menu }
        })");
        QObject* menu = harness.object("menu");
        QVariant item = QVariant::fromValue<QObject*>(harness.window->contentItem());
        const auto show = [&](const QString& text) {
            const QVariantList actions{QVariantMap{{QStringLiteral("text"), text}}};
            REQUIRE(QMetaObject::invokeMethod(menu, "show", Q_ARG(QVariant, QVariant(actions)), Q_ARG(QVariant, item),
                                              Q_ARG(QVariant, QVariant(QPointF(10, 10)))));
            QCoreApplication::processEvents();
        };
        show(QStringLiteral("Play"));
        const qreal narrow = menu->property("implicitWidth").toReal();
        CHECK(narrow == 220);
        QMetaObject::invokeMethod(menu, "close");
        show(QStringLiteral("A rather long menu entry that needs much more room than the others"));
        REQUIRE(menu->property("implicitWidth").toReal() > narrow);
        QMetaObject::invokeMethod(menu, "close");
        show(QStringLiteral("Play"));
        CHECK(menu->property("implicitWidth").toReal() == narrow);
    }

    TEST_CASE("the full player's flying cover is decoded once, not on every animation frame")
    {
        std::atomic<int> requests = 0;
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): QQmlEngine takes ownership of image providers
        auto* cover = new CountingCoverProvider(requests);
        const WindowHarness harness(R"(Window {
            width: 1000; height: 800; visible: true
            property Item player: player
            FullPlayer { id: player; anchors.fill: parent; sourceRect: Qt.rect(20, 720, 64, 64) }
        })",
                                    cover);
        QQuickItem* player = harness.item("player");
        REQUIRE(test::waitUntil([&] { return requests > 0; }));
        QTest::qWait(200);
        const int before = requests;
        player->setProperty("open", true);
        REQUIRE(test::waitUntil([&] { return player->property("shown").toReal() == 1.0; }));
        QTest::qWait(200);
        CHECK_MESSAGE(requests - before <= 1, "cover requests during the flight: ", requests - before);
    }

    TEST_CASE("the full player's flying cover starts on the bar's cover when the player sits below a title bar")
    {
        const WindowHarness harness(R"(Window {
            width: 1000; height: 800; visible: true
            property Item player: player
            FullPlayer {
                id: player
                anchors.fill: parent
                anchors.topMargin: 48  // the custom title bar
                sourceRect: Qt.rect(20, 720, 64, 64)  // window coordinates
            }
        })");
        QQuickItem* player = harness.item("player");
        auto* flyer = player->findChild<QQuickItem*>(QStringLiteral("flyingCover"));
        REQUIRE(flyer != nullptr);
        REQUIRE(player->property("shown").toReal() == 0.0);
        const QPointF start = flyer->mapToScene(QPointF(0, 0));
        CHECK(start.x() == doctest::Approx(20));
        CHECK(start.y() == doctest::Approx(720));
    }

    TEST_CASE("a narrow player bar keeps room for the scrubber and never overlaps the times")
    {
        // 612 px: the minimum window width with the expanded navigation rail.
        const WindowHarness harness(R"(Window {
            width: 900; height: 200; visible: true
            property Item bar: bar
            NowPlayingBar { id: bar; width: 612; height: 80 }
        })");
        QQuickItem* bar = harness.item("bar");
        const auto child = [&](const char* id) {
            // The ids inside NowPlayingBar.qml live in the context of its own items.
            QQmlContext* context = qmlContext(bar->property("coverItem").value<QObject*>());
            REQUIRE(context != nullptr);
            auto* item = qobject_cast<QQuickItem*>(context->objectForName(QString::fromLatin1(id)));
            REQUIRE(item != nullptr);
            return item;
        };
        const auto check = [&] {
            QQuickItem* waveform = child("waveform");
            QQuickItem* elapsed = child("elapsed");
            QQuickItem* total = child("total");
            CHECK(waveform->width() >= 48);
            if (elapsed->isVisible() && total->isVisible()) {
                CHECK(elapsed->x() + elapsed->width() <= total->x());
            }
            CHECK(waveform->x() + waveform->width() <= total->x() + (total->isVisible() ? 0 : total->width()));
        };
        check();
        bar->setWidth(1200);  // wide again: the times come back
        CHECK(child("elapsed")->isVisible());
        CHECK(child("total")->isVisible());
        check();
    }

    TEST_CASE("the keypad Delete key clears a shortcut like the main Delete key")
    {
        // Like the real KeyboardController: only Ctrl/Shift/Alt/Meta count, so keypad Delete would read as "Del".
        stub::singleton("Shortcuts", QStringLiteral("({fromKeyEvent: (key, mods) => key === Qt.Key_Delete ? 'Del' "
                                                    ": 'Key' + key, display: text => text})"));
        const WindowHarness harness(R"(Window {
            id: window
            width: 300; height: 100; visible: true
            property Item recorder: recorder
            property var picks: []
            ShortcutRecorder {
                id: recorder
                value: "Ctrl+K"
                onPicked: sequence => window.picks = window.picks.concat([sequence])
            }
        })");
        QQuickItem* recorder = harness.item("recorder");
        harness.window->requestActivate();
        recorder->forceActiveFocus();
        REQUIRE(test::waitUntil([&] { return recorder->hasActiveFocus(); }));
        for (const auto modifiers : {Qt::KeyboardModifiers(Qt::KeypadModifier), Qt::KeyboardModifiers()}) {
            recorder->setProperty("recording", true);
            QTest::keyClick(harness.window.get(), Qt::Key_Delete, modifiers);
            CHECK_FALSE(recorder->property("recording").toBool());
        }
        const QStringList picks = harness.window->property("picks").toStringList();
        const QStringList cleared{QString(), QString()};
        CHECK_MESSAGE(picks == cleared, "picked: ", picks.join(u',').toStdString());
    }

    TEST_CASE("a toast with a long message and an action keeps its buttons inside the panel")
    {
        const WindowHarness harness(R"(Window {
            width: 800; height: 300; visible: true
            property Item toast: toast
            Toast {
                id: toast
                kind: "error"
                message: "The graphics driver stopped responding. ".repeat(5)
                actionLabel: "Restart now"
            }
        })");
        QQuickItem* toast = harness.item("toast");
        QQuickItem* row = nullptr;
        for (auto* child : toast->findChildren<QQuickItem*>()) {
            if (QByteArrayView(child->metaObject()->className()) == "QQuickRow" && child->childItems().size() == 4) {
                row = child;  // icon, message, action, dismiss
            }
        }
        REQUIRE(row != nullptr);
        QCoreApplication::processEvents();
        CHECK(toast->width() <= 480);
        CHECK(row->x() + row->width() <= toast->width() - 16);
    }

    TEST_CASE("the navigation pill is hidden while the current page is not in the rail")
    {
        const WindowHarness harness(R"(Window {
            width: 100; height: 400; visible: true
            property Item rail: rail
            NavRail {
                id: rail
                width: 72; height: 400
                destinations: [{id: "library", icon: "library", label: "Library"}]
                footer: [{id: "settings", icon: "settings", label: "Settings"}]
                current: "library"
            }
        })");
        QQuickItem* rail = harness.item("rail");
        auto* pill = rail->findChild<QQuickItem*>(QStringLiteral("selectionPill"));
        REQUIRE(pill != nullptr);
        CHECK(pill->isVisible());
        rail->setProperty("current", QStringLiteral("home"));  // hidden from the rail by the user
        CHECK_FALSE(pill->isVisible());
        rail->setProperty("current", QStringLiteral("settings"));
        CHECK(pill->isVisible());
    }

    TEST_CASE("queue edits keep the queue list's scroll position")
    {
        media::PlaybackQueue queue;
        media::QueueModel model(queue);
        queue.replace(queueItems(200), 0);
        model.reload();
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Warning);
        QQmlEngine::setObjectOwnership(&model, QQmlEngine::CppOwnership);
        const WindowHarness harness(R"(Window {
            width: 300; height: 400; visible: true
            property Item list: list
            QueueList { id: list; anchors.fill: parent }
        })",
                                    nullptr, [&model](QJSEngine* js) {
                                        QJSValue player = js->newObject();
                                        player.setProperty(QStringLiteral("queue"), js->newQObject(&model));
                                        return player;
                                    });
        QQuickItem* list = harness.item("list");
        REQUIRE(test::waitUntil([&] { return list->property("count").toInt() == 200; }));
        // The track at the top of the view (its number in the title).
        const auto topTrack = [&] {
            int row = -1;
            QMetaObject::invokeMethod(list, "indexAt", Q_RETURN_ARG(int, row), Q_ARG(qreal, 10),
                                      Q_ARG(qreal, list->property("contentY").toReal() + 1));
            return model.data(model.index(row), media::QueueModel::TitleRole).toString().section(u' ', 1).toInt();
        };
        list->setProperty("contentY", 150 * 40);
        QCoreApplication::processEvents();
        REQUIRE(topTrack() == 150);

        // The view stays where it is; a row added or removed above shifts the content by that row only.
        const auto stays = [&](int expected) {
            QCoreApplication::processEvents();
            CHECK(list->property("contentY").toReal() == 150 * 40);
            CHECK(std::abs(topTrack() - expected) <= 1);
        };
        REQUIRE(queue.remove(160));  // the hover "x" on a row below
        model.reload();
        stays(150);
        REQUIRE(queue.move(170, 1));  // "Play next"
        model.reload();
        stays(150);
        REQUIRE(queue.remove(5));  // above the visible rows
        model.reload();
        stays(150);
        queue.append(queueItems(3));
        model.reload();
        stays(150);
        CHECK(model.data(model.index(model.rowCount() - 1), media::QueueModel::TitleRole).toString() ==
              QStringLiteral("Track 2"));

        // A new queue still brings its current item into view.
        list->setProperty("contentY", 0);
        queue.replace(queueItems(200), 120);
        model.reload();
        CHECK(test::waitUntil([&] {
            const qreal top = list->property("contentY").toReal();
            return top <= 120 * 40 && 121 * 40 <= top + 400;
        }));
    }
}

namespace {

// An API client that records the URLs it is asked for and never answers (the lists stay loading).
class RecordingClient : public api::IApiClient {
public:
    net::RequestHandle send(api::ApiRequest request, QObject* /*context*/, api::Decoder /*decode*/) override
    {
        urls.push_back(request.url);
        return {};
    }
    net::RequestHandle lookupTracks(std::vector<api::Id> /*ids*/, QObject* /*context*/,
                                    std::function<void(Result<std::vector<api::Track>>)> /*done*/) override
    {
        return {};
    }
    [[nodiscard]] bool asked(const QString& path) const
    {
        return std::ranges::any_of(urls, [&](const QUrl& url) { return url.path() == path; });
    }

    std::vector<QUrl> urls;
};

}  // namespace

TEST_SUITE("ui.pages")
{
    TEST_CASE("a user page reused for another user loads that user's playlists on the Playlists tab")
    {
        static const bool registered = [] {
            qmlRegisterUncreatableType<models::UserDetail>("Noctara", 1, 0, "UserDetail", QStringLiteral("test"));
            qmlRegisterUncreatableType<models::CollectionModel>("Noctara", 1, 0, "CollectionModel",
                                                                QStringLiteral("test"));
            return true;
        }();
        (void)registered;
        RecordingClient client;
        models::UserDetail first(client, 1, 20);
        models::UserDetail second(client, 2, 20);
        QQmlEngine::setObjectOwnership(&first, QQmlEngine::CppOwnership);
        QQmlEngine::setObjectOwnership(&second, QQmlEngine::CppOwnership);
        // Like Main.qml's detail Loader: one user detail replacing another keeps the same UserPage.
        const WindowHarness harness(R"(import "../pages"
        Window {
            id: root
            width: 800; height: 600; visible: true
            property QtObject user: null
            property Item page: loader.item
            Loader {
                id: loader
                anchors.fill: parent
                active: root.user !== null
                sourceComponent: Component { UserPage { detail: root.user } }
            }
        })");
        harness.window->setProperty("user", QVariant::fromValue<QObject*>(&first));
        REQUIRE(test::waitUntil([&] { return harness.item("page") != nullptr; }));
        QQuickItem* page = harness.item("page");
        page->setProperty("tab", QStringLiteral("playlists"));
        CHECK(client.asked(QStringLiteral("/users/1/playlists")));

        harness.window->setProperty("user", QVariant::fromValue<QObject*>(&second));
        REQUIRE(harness.item("page") == page);
        CHECK(page->property("tab").toString() == QStringLiteral("playlists"));
        CHECK(test::waitUntil([&] { return client.asked(QStringLiteral("/users/2/playlists")); }, 1000));
    }
}

TEST_SUITE("ui.settings-panes")
{
    TEST_CASE("theme names and issue lines from a theme file are shown as plain text")
    {
        // A theme's name and its (raw TOML key) issue lines are untrusted: rich text would load <img> sources.
        stub::singleton("Themes", QStringLiteral(R"js(({
            themes: [{id: 'a', name: "<img src='image://cover/name'>Aurora", usable: true},
                     {id: 'b', name: "<img src='image://cover/unusable'>Broken", usable: false}],
            selected: 'a',
            selectedIssues: ["<img src='image://cover/issue'> is not a known token"],
            select: id => {}, importFile: url => {}, exportTheme: (id, url) => {}, openFolder: () => {}
        }))js"));
        std::atomic<int> requests = 0;
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): QQmlEngine takes ownership of image providers
        auto* cover = new CountingCoverProvider(requests);
        const WindowHarness harness(R"(import "../settings"
        Window {
            width: 600; height: 400; visible: true
            property Item picker: picker
            ThemePicker { id: picker }
        })",
                                    cover);
        QQuickItem* combo = nullptr;
        for (auto* child : harness.item("picker")->findChildren<QQuickItem*>()) {
            if (child->inherits("QQuickComboBox")) {
                combo = child;
            }
        }
        REQUIRE(combo != nullptr);
        REQUIRE(combo->property("displayText").toString().contains(QStringLiteral("Aurora")));
        auto* popup = combo->property("popup").value<QObject*>();
        REQUIRE(popup != nullptr);
        REQUIRE(QMetaObject::invokeMethod(popup, "open"));
        REQUIRE(test::waitUntil([&] { return popup->property("opened").toBool(); }));
        QTest::qWait(300);
        CHECK_MESSAGE(requests == 0, "image requests from theme text: ", requests.load());
    }

    TEST_CASE("Enter in the empty proxy password field keeps the saved credentials")
    {
        stub::singleton("Network", QStringLiteral(R"js((function() {
            const network = {proxyUsername: 'u', hasProxyCredentials: true, keyringAvailable: true,
                             secretsPersistent: true, calls: []};
            network.saveProxyCredentials = (user, password) => network.calls.push('save ' + user + ':' + password);
            network.forgetProxyCredentials = () => network.calls.push('forget');
            return network;
        })())js"));
        const WindowHarness harness(R"(import "../settings/network"
        Window {
            width: 600; height: 300; visible: true
            property Item credentials: credentials
            function calls() { return Network.calls.join(',') }
            ProxyCredentials { id: credentials; width: 600 }
        })");
        QQuickItem* password = nullptr;
        for (auto* child : harness.item("credentials")->findChildren<QQuickItem*>()) {
            if (child->inherits("QQuickTextField") && child->property("echoMode").toInt() == 2) {  // Password
                password = child;
            }
        }
        REQUIRE(password != nullptr);
        REQUIRE(password->property("text").toString().isEmpty());
        REQUIRE(QMetaObject::invokeMethod(password, "accepted"));
        QVariant calls;
        REQUIRE(QMetaObject::invokeMethod(harness.window.get(), "calls", Q_RETURN_ARG(QVariant, calls)));
        CHECK_MESSAGE(calls.toString().isEmpty(), "credential calls: ", calls.toString());

        password->setProperty("text", QStringLiteral("secret"));  // a typed password is still saved on Enter
        REQUIRE(QMetaObject::invokeMethod(password, "accepted"));
        REQUIRE(QMetaObject::invokeMethod(harness.window.get(), "calls", Q_RETURN_ARG(QVariant, calls)));
        CHECK(calls.toString() == QStringLiteral("save u:secret"));
    }
}
