#include "api/ApiService.hpp"
#include "api/Session.hpp"

#include "auth/BrowserCookieSource.hpp"
#include "auth/SecretStore.hpp"
#include "cache/CacheService.hpp"
#include "core/Log.hpp"
#include "core/LsanSuppressions.hpp"
#include "core/Paths.hpp"
#include "drm/DrmService.hpp"
#include "gfx/CrashSentinel.hpp"
#include "gfx/FrameMonitor.hpp"
#include "gfx/GraphicsBootstrap.hpp"
#include "gfx/PowerMonitor.hpp"
#include "gfx/Relaunch.hpp"
#include "i18n/ErrorMessages.hpp"
#include "i18n/Formatter.hpp"
#include "i18n/LanguageManager.hpp"
#include "media/AudioOutput.hpp"
#include "media/CoverFiles.hpp"
#include "media/Mpris.hpp"
#include "media/NativeBackend.hpp"
#include "media/PlaybackController.hpp"
#include "media/QueueModel.hpp"
#include "net/CurlSupport.hpp"
#include "net/NetworkMonitor.hpp"
#include "net/NetworkService.hpp"
#include "net/ProxyRouter.hpp"
#include "settings/ConfigFile.hpp"
#include "settings/Keys.hpp"
#include "settings/Overrides.hpp"
#include "settings/SchemaDoc.hpp"
#include "settings/SettingsStore.hpp"
#include "theme/AccentEngine.hpp"
#include "theme/FontLoader.hpp"
#include "theme/Theme.hpp"
#include "theme/ThemeLibrary.hpp"
#include "ui/AccountController.hpp"
#include "ui/AppController.hpp"
#include "ui/ArtworkProvider.hpp"
#include "ui/CatalogController.hpp"
#include "ui/CoverImageProvider.hpp"
#include "ui/EffectsController.hpp"
#include "ui/IconProvider.hpp"
#include "ui/KeyboardController.hpp"
#include "ui/LayoutController.hpp"
#include "ui/NetworkController.hpp"
#include "ui/NoiseProvider.hpp"
#include "ui/PowerStatus.hpp"
#include "ui/SettingsController.hpp"
#include "ui/SettingsModel.hpp"
#include "ui/StorageController.hpp"
#include "ui/ThemesController.hpp"
#include "ui/TrackNotifier.hpp"
#include "ui/TrayIcon.hpp"
#include "ui/WaveformItem.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QImage>
#include <QPointer>
#include <QProcessEnvironment>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>

#include <csignal>
#include <cstdio>
#include <memory>
#include <print>
#include <span>
#include <string_view>

using namespace noctara;

namespace {

bool hasFlag(std::span<char*> args, std::string_view flag)
{
    return std::ranges::any_of(args,
                               [flag](const char* arg) { return arg != nullptr && std::string_view(arg) == flag; });
}

struct CommandLine {
    QString configPath;
    bool printConfig = false;
    bool resetConfig = false;
    bool safeMode = false;
    bool pgoScenario = false;
    QString screenshot;      // dev/testing: render, save the window image, quit
    QString page;            // dev/testing: initial page id
    QString navigate;        // dev/testing: pages to switch to after startup, comma-separated
    QString artwork;         // dev/testing: image file used as the current track's artwork
    QString play;            // dev/testing: a SoundCloud link to play after startup
    bool smokeTest = false;  // visit every page and settings category; fail on any warning
    QStringList assignments;
    QStringList aliases;
};

CommandLine parseCommandLine(const QCoreApplication& app, const settings::Schema& schema)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Noctara: a native SoundCloud client"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption config(QStringLiteral("config"), QStringLiteral("Use <path> as the user config file."),
                                    QStringLiteral("path"));
    const QCommandLineOption print(QStringLiteral("print-config"),
                                   QStringLiteral("Print the effective configuration with sources and exit."));
    const QCommandLineOption reset(QStringLiteral("reset-config"),
                                   QStringLiteral("Back up the config file, write fresh defaults and exit."));
    const QCommandLineOption safe(QStringLiteral("safe-mode"),
                                  QStringLiteral("Start with software rendering and no shader effects."));
    const QCommandLineOption disableGpu(QStringLiteral("disable-gpu"), QStringLiteral("Same as --gpu=off."));
    const QCommandLineOption set(QStringLiteral("set"),
                                 QStringLiteral("Override a setting: --set key=value (repeatable)."),
                                 QStringLiteral("key=value"));
    QCommandLineOption pgo(QStringLiteral("pgo-scenario"), QStringLiteral("Run the scripted PGO scenario and exit."));
    pgo.setFlags(QCommandLineOption::HiddenFromHelp);
    QCommandLineOption screenshot(QStringLiteral("screenshot"), QStringLiteral("Save a window capture and exit."),
                                  QStringLiteral("file"));
    screenshot.setFlags(QCommandLineOption::HiddenFromHelp);
    QCommandLineOption page(QStringLiteral("page"), QStringLiteral("Initial page."), QStringLiteral("id"));
    page.setFlags(QCommandLineOption::HiddenFromHelp);
    QCommandLineOption smoke(QStringLiteral("smoke-test"), QStringLiteral("Visit all pages and exit."));
    smoke.setFlags(QCommandLineOption::HiddenFromHelp);
    QCommandLineOption navigate(QStringLiteral("navigate"), QStringLiteral("Switch page after startup."),
                                QStringLiteral("id"));
    navigate.setFlags(QCommandLineOption::HiddenFromHelp);
    QCommandLineOption artwork(QStringLiteral("artwork"), QStringLiteral("Use an image file as the artwork."),
                               QStringLiteral("file"));
    artwork.setFlags(QCommandLineOption::HiddenFromHelp);
    QCommandLineOption play(QStringLiteral("play"), QStringLiteral("Play a SoundCloud link."), QStringLiteral("url"));
    play.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOptions(
        {config, print, reset, safe, disableGpu, set, pgo, screenshot, page, smoke, navigate, artwork, play});

    std::vector<QCommandLineOption> aliasOptions;
    for (const settings::Definition& def : schema.definitions()) {
        if (!def.cliAlias.empty()) {
            const QString name = QString::fromLatin1(def.cliAlias.data(), static_cast<qsizetype>(def.cliAlias.size()));
            aliasOptions.emplace_back(
                name,
                QStringLiteral("Shortcut for --set %1=<value>.")
                    .arg(QString::fromLatin1(def.key.data(), static_cast<qsizetype>(def.key.size()))),
                QStringLiteral("value"));
            parser.addOption(aliasOptions.back());
        }
    }
    parser.process(app);

    CommandLine cli;
    cli.configPath = parser.value(config);
    cli.printConfig = parser.isSet(print);
    cli.resetConfig = parser.isSet(reset);
    cli.safeMode = parser.isSet(safe);
    cli.pgoScenario = parser.isSet(pgo);
    cli.screenshot = parser.value(screenshot);
    cli.page = parser.value(page);
    cli.smokeTest = parser.isSet(smoke);
    cli.navigate = parser.value(navigate);
    cli.artwork = parser.value(artwork);
    cli.play = parser.value(play);
    cli.assignments = parser.values(set);
    for (const QCommandLineOption& option : aliasOptions) {
        if (parser.isSet(option)) {
            cli.aliases.append(option.names().front() + u'=' + parser.value(option));
        }
    }
    if (parser.isSet(disableGpu)) {
        cli.aliases.append(QStringLiteral("gpu=off"));
    }
    return cli;
}

void printUtf8(const QString& text)
{
    const QByteArray bytes = text.toUtf8();
    (void)std::fwrite(bytes.constData(), 1, static_cast<std::size_t>(bytes.size()), stdout);
}

ui::Availability availability(const gfx::GraphicsDecision& decision, const net::CurlFeatures& curl)
{
    ui::Availability availability;
    if (!curl.http3) {
        availability.disabledOptions[QStringLiteral("network.connection.http_version")].insert(
            QStringLiteral("3"), QT_TRANSLATE_NOOP("Settings", "The installed libcurl was built without HTTP/3."));
    }
    if (!decision.hardware) {
        static const char* const reason =
            QT_TRANSLATE_NOOP("Settings", "Needs hardware acceleration, which is currently off.");
        auto& glass = availability.disabledOptions[QStringLiteral("appearance.glass.style")];
        glass.insert(QStringLiteral("kawase"), reason);
        glass.insert(QStringLiteral("gaussian"), reason);
        availability.disabledOptions[QStringLiteral("appearance.glass.surface")].insert(QStringLiteral("liquid"),
                                                                                        reason);
        availability.disabledSettings.insert(QStringLiteral("appearance.glass.blur_strength"), reason);
        availability.disabledSettings.insert(QStringLiteral("appearance.backdrop.animate"), reason);
        availability.disabledSettings.insert(QStringLiteral("appearance.glass.quality"), reason);
    } else if (!decision.liquidGlassAllowed) {
        availability.disabledOptions[QStringLiteral("appearance.glass.surface")].insert(
            QStringLiteral("liquid"), QT_TRANSLATE_NOOP("Settings", "Not available on this graphics device."));
    }
    return availability;
}

// Visits every page and settings category, rendering each, then exits with 0 only if nothing logged a
// warning. Used by `xmake smoke` (software and hardware renderers, both themes, both languages).
void runSmokeTest(const QQmlApplicationEngine& engine, const ui::SettingsController& settingsUi)
{
    QObject* root = engine.rootObjects().constFirst();
    auto* window = qobject_cast<QQuickWindow*>(root);
    QStringList steps{QStringLiteral("home"),  QStringLiteral("search"),  QStringLiteral("library"),
                      QStringLiteral("likes"), QStringLiteral("history"), QStringLiteral("charts")};
    for (const QVariant& category : settingsUi.categories()) {
        steps.append(QStringLiteral("settings/") + category.toMap().value(QStringLiteral("id")).toString());
    }
    const int baseline = log::warningCount();
    // The player bar and the queue drawer appear without a track, so their software and GPU paths render too.
    root->setProperty("playerPreview", true);
    steps.append(QStringLiteral("queue"));
    steps.append(QStringLiteral("cheat-sheet"));
    steps.append(QStringLiteral("full-player"));
    const auto index = std::make_shared<qsizetype>(0);
    auto* timer = new QTimer(root);  // NOLINT(cppcoreguidelines-owning-memory): owned by its QObject parent
    timer->setInterval(150);
    QObject::connect(timer, &QTimer::timeout, root, [=] {
        if (window != nullptr) {
            (void)window->grabWindow();  // force a full render of the current step
        }
        if (*index == steps.size()) {
            const int warnings = log::warningCount() - baseline;
            std::println(stderr, "smoke test: {} steps, {} warnings", steps.size(), warnings);
            QCoreApplication::exit(warnings == 0 ? 0 : 2);
            return;
        }
        const QString& step = steps.at((*index)++);
        if (step == u"queue") {
            root->setProperty("queuePanel", true);
            return;
        }
        if (step == u"cheat-sheet") {
            root->setProperty("cheatSheet", true);
            return;
        }
        if (step == u"full-player") {
            root->setProperty("cheatSheet", false);
            root->setProperty("fullPlayer", true);
            return;
        }
        root->setProperty("page", step.section(u'/', 0, 0));
        if (step.contains(u'/')) {
            root->setProperty("settingsCategory", step.section(u'/', 1, 1));
        }
    });
    timer->start();
}

}  // namespace

int main(int argc, char** argv)
{
    gfx::RelaunchGuard relaunch;  // destroyed last: performs a requested self-exec after full shutdown
    // A peer closing a connection must surface as a write error, never terminate the process.
    (void)std::signal(SIGPIPE, SIG_IGN);
    log::install();
    QCoreApplication::setApplicationName(QString::fromLatin1(kAppId));
    QCoreApplication::setApplicationVersion(QStringLiteral(NOCTARA_VERSION));
    // The desktop file name makes Qt register the app id with the desktop portal, which fails (and logs a
    // warning) unless noctara.desktop is installed, e.g. when running from the build tree.
    if (!QStandardPaths::locate(QStandardPaths::ApplicationsLocation, QString::fromLatin1(kAppId) + u".desktop")
             .isEmpty()) {
        QGuiApplication::setDesktopFileName(QString::fromLatin1(kAppId));
    }

    const std::span<char*> rawArgs(argv, static_cast<std::size_t>(argc));
    const bool headless = hasFlag(rawArgs, "--print-config") || hasFlag(rawArgs, "--reset-config") ||
                          hasFlag(rawArgs, "--help") || hasFlag(rawArgs, "-h") || hasFlag(rawArgs, "--version");
    // QApplication (not QGuiApplication) for the tray icon's menu; the UI itself is Qt Quick.
    const std::unique_ptr<QCoreApplication> app =
        headless ? std::make_unique<QCoreApplication>(argc, argv) : std::make_unique<QApplication>(argc, argv);

    const settings::Schema& schema = settings::Schema::builtin();
    const CommandLine cli = parseCommandLine(*app, schema);
    const AppPaths paths = AppPaths::standard();

    settings::SettingsStore store(schema);
    settings::ConfigFile config(store, cli.configPath.isEmpty() ? paths.userConfigFile() : cli.configPath,
                                paths.systemConfigFile);
    if (cli.resetConfig) {
        if (auto reset = config.resetToDefaults(); !reset) {
            std::println(stderr, "reset failed: {}", reset.error().describe());
            return 1;
        }
        printUtf8(QStringLiteral("Configuration reset; previous file kept as %1.bak\n").arg(config.path()));
        return 0;
    }
    settings::ConfigIssues issues = config.load();
    const auto environment = settings::collectEnvironment(schema, QProcessEnvironment::systemEnvironment());
    const auto commandLine = settings::collectCommandLine(schema, cli.assignments, cli.aliases);
    store.replaceLayer(settings::Layer::Environment, environment.values);
    store.replaceLayer(settings::Layer::CommandLine, commandLine.values);
    issues.append(environment.issues);
    issues.append(commandLine.issues);
    for (const auto& issue : environment.issues + commandLine.issues) {
        qCWarning(lcSettings).noquote() << issue.logText();
    }

    if (cli.printConfig) {
        config.flush();
        printUtf8(settings::effectiveConfigReport(store));
        return 0;
    }

    // The transport cannot work without TLS and HTTP/2 in libcurl: refuse to start rather than misbehave.
    const auto curl = net::initialiseCurl();
    if (!curl) {
        std::println(stderr, "noctara: {}", curl.error().describe());
        return 1;
    }

    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/app/noctara.svg")));

    gfx::CrashSentinel sentinel(paths.stateDir + QStringLiteral("/graphics-init.sentinel"));
    const gfx::GraphicsDecision decision =
        gfx::bootstrapGraphics(store, sentinel, {.cacheDir = paths.cacheDir, .safeModeFlag = cli.safeMode});
    gfx::GraphicsInfo graphics(decision);

    i18n::LanguageManager languages(store);
    i18n::Formatter formatter(store, languages);
    const theme::FontFamilies fonts = theme::FontLoader::loadBundled();
    theme::Theme theme(store, fonts);
    theme.setBlurAvailable(decision.hardware);
    QGuiApplication::setFont(theme.font().body);

    // User themes (<config>/themes/*.toml): data only, hot-reloaded; the selected one overrides tokens.
    theme::ThemeLibrary themeLibrary(store, paths.themesDir());
    QObject::connect(&themeLibrary, &theme::ThemeLibrary::activeChanged, &theme,
                     [&theme, &themeLibrary] { theme.setUserTheme(themeLibrary.active()); });

    theme::AccentEngine accent(store);
    QObject::connect(&accent, &theme::AccentEngine::seedChanged, &theme,
                     [&theme](std::optional<color::OkLch> seed) { theme.setArtworkSeed(seed); });

    ui::SettingsController settingsUi(store, config, languages);
    settingsUi.setAvailability(availability(decision, *curl));
    gfx::PowerMonitor power;
    const ui::PowerStatusPresenter powerStatus(power, store, languages, settingsUi);
    // Declared before the QML engine: the window's render thread reports to it until the window is gone.
    gfx::FrameMonitor frames;
    ui::EffectsController effects(store, theme, power, frames, graphics, languages, settingsUi);
    ui::AppController appController(store, graphics, relaunch, languages);
    ui::ThemesController themesUi(themeLibrary, store, languages);
    QObject::connect(
        &themesUi, &ui::ThemesController::notice, &appController,
        [&appController](const QString& kind, const QString& message) { appController.showToast(kind, message); });
    ui::KeyboardController keyboard(store, languages, settingsUi);
    ui::LayoutController layout(store, paths.stateDir + QStringLiteral("/window.json"));
    // All HTTP goes through this service; it runs on its own thread and follows the network settings live.
    net::NetworkService network(store, net::processEnvironment(), true);
    net::NetworkMonitor networkMonitor;
    auth::SecretServiceStore secrets(paths.stateDir + QStringLiteral("/secrets"));
    ui::NetworkController networkUi(network, &networkMonitor, secrets, languages, settingsUi);
    QObject::connect(
        &networkUi, &ui::NetworkController::notice, &appController,
        [&appController](const QString& kind, const QString& message) { appController.showToast(kind, message); });
    // Caches (audio RAM + disk, images, HTTP) on their own thread; the API client will use cache.transport().
    cache::CacheService cache(store, network.transport(), paths.cacheDir);
    ui::StorageController storageUi(cache, languages, settingsUi);
    QObject::connect(
        &storageUi, &ui::StorageController::notice, &appController,
        [&appController](const QString& kind, const QString& message) { appController.showToast(kind, message); });
    // SoundCloud's api-v2 on its own thread, over the caching transport. The session token comes from auth/ (f).
    api::Session session;
    api::ApiService soundcloud(store, cache.transport(), session, paths.stateDir);
    auth::BrowserCookieSource browserCookies;
    // Browser discovery never reads cookies; Account asks for consent before importing the selected profile.
    ui::AccountController account(soundcloud.client(), session, secrets, &browserCookies);
    ui::CatalogController catalog(soundcloud, account);
    QObject::connect(&account, &ui::AccountController::notice, &appController, &ui::AppController::showToast);

    // Native playback: the audio cache feeds FFmpeg, the mixer feeds the default output device (opened on the
    // first track). Protected tracks play in the castlabs Electron helper, started on the first one.
    media::QtAudioOutput audioOutput;
    media::NativeBackend nativePlayback(cache.audio(), audioOutput);
    const QString cacheRoot = cache.status().root.isEmpty() ? paths.cacheDir : cache.status().root;
    drm::DrmService protectedPlayback(store, network, cacheRoot + QStringLiteral("/electron"),
                                      QCoreApplication::applicationDirPath());
    const auto showProtectedStatus = [&settingsUi, &protectedPlayback] {
        settingsUi.setRowStatus(QString::fromLatin1(settings::keys::ProtectedEnabled), protectedPlayback.statusText());
    };
    showProtectedStatus();
    QObject::connect(&protectedPlayback, &drm::DrmService::statusChanged, &settingsUi, showProtectedStatus);
    QObject::connect(&languages, &i18n::LanguageManager::languageChanged, &settingsUi, showProtectedStatus);
    QObject::connect(&storageUi, &ui::StorageController::clearingAll, &protectedPlayback,
                     &drm::DrmService::clearProfile);
    // Once per session: protected tracks play although some network settings do not apply to them.
    QObject::connect(&protectedPlayback.host(), &drm::DrmHost::ready, &appController,
                     [&appController, &protectedPlayback, warned = false] mutable {
                         if (const QString warning = protectedPlayback.networkWarning();
                             !warning.isEmpty() && !warned) {
                             warned = true;
                             appController.showToast(QStringLiteral("warning"), warning);
                         }
                     });
    media::PlaybackController player({.settings = store,
                                      .api = soundcloud.client(),
                                      .cache = &cache,
                                      .native = nativePlayback,
                                      .protectedBackend = &protectedPlayback.backend(),
                                      .metered = [&network] { return network.meteredActive(); },
                                      .statePath = paths.stateDir + QStringLiteral("/session.json")});
    QObject::connect(&player, &media::PlaybackController::problem, &appController, [&appController](int code) {
        appController.showToast(QStringLiteral("error"), i18n::errorMessage(static_cast<ErrorCode>(code)));
    });
    // The current track's cover drives the accent palette and the backdrop.
    QObject::connect(&player, &media::PlaybackController::trackChanged, &accent, [&player, &accent, &cache] {
        const QString url = player.artworkUrl();
        if (url.isEmpty()) {
            return;
        }
        cache.images().request(QUrl(url), QSize(500, 500), &accent, [&accent, &player, url](Result<QImage> image) {
            if (image && url == player.artworkUrl()) {
                accent.setArtwork(url, *image);
            }
        });
    });
    QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtimeDir.isEmpty()) {
        runtimeDir = paths.cacheDir;
    }
    media::CoverFiles coverFiles(cache.images(), runtimeDir + QStringLiteral("/noctara/covers-") +
                                                     QString::number(QCoreApplication::applicationPid()));
    media::MprisService mpris(player, &coverFiles);
    mpris.setEnabled(store.boolValue(settings::keys::MediaControls));
    QObject::connect(&store, &settings::SettingsStore::valueChanged, &mpris, [&mpris, &store](const QString& key) {
        if (key == QLatin1StringView(settings::keys::MediaControls)) {
            mpris.setEnabled(store.boolValue(settings::keys::MediaControls));
        }
    });
    QPointer<QQuickWindow> mainWindow;
    const auto showWindow = [&mainWindow] {
        if (mainWindow) {
            mainWindow->show();
            mainWindow->raise();
            mainWindow->requestActivate();
        }
    };
    QObject::connect(&mpris, &media::MprisService::raiseRequested, &mpris, showWindow);
    ui::TrayIcon tray(player, store, languages);
    QObject::connect(&tray, &ui::TrayIcon::showRequested, &tray, showWindow);
    QObject::connect(&keyboard, &ui::KeyboardController::globalActivated, &player,
                     [&player, showWindow](const QString& action) {
                         if (action == u"play_pause") {
                             player.playPause();
                         } else if (action == u"next") {
                             player.next();
                         } else if (action == u"previous") {
                             player.previous();
                         } else if (action == u"show") {
                             showWindow();
                         }
                     });
    const ui::TrackNotifier notifier(player, store, &coverFiles,
                                     [&mainWindow] { return mainWindow && mainWindow->isActive(); });
    QObject::connect(&config, &settings::ConfigFile::reloadIssues, &appController,
                     &ui::AppController::reportConfigIssues);
    config.setHotReloadEnabled(true);

    QQmlApplicationEngine engine;
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): QQmlEngine takes ownership of image providers
    engine.addImageProvider(QStringLiteral("icon"), new ui::IconProvider);
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): QQmlEngine takes ownership of image providers
    engine.addImageProvider(QStringLiteral("noise"), new ui::NoiseProvider);
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): QQmlEngine takes ownership of image providers
    engine.addImageProvider(QStringLiteral("artwork"), new ui::ArtworkProvider(accent));
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): QQmlEngine takes ownership of image providers
    engine.addImageProvider(QStringLiteral("cover"), new ui::CoverImageProvider(cache.images()));
    qmlRegisterSingletonInstance("Noctara", 1, 0, "Theme", &theme);
    qmlRegisterSingletonInstance("Noctara", 1, 0, "Format", &formatter);
    qmlRegisterSingletonInstance("Noctara", 1, 0, "SettingsUi", &settingsUi);
    qmlRegisterSingletonInstance("Noctara", 1, 0, "App", &appController);
    qmlRegisterSingletonInstance("Noctara", 1, 0, "Graphics", &graphics);
    qmlRegisterSingletonInstance("Noctara", 1, 0, "Language", &languages);
    qmlRegisterSingletonInstance("Noctara", 1, 0, "Performance", &effects);
    qmlRegisterSingletonInstance("Noctara", 1, 0, "Artwork", &accent);
    qmlRegisterSingletonInstance("Noctara", 1, 0, "Network", &networkUi);
    qmlRegisterSingletonInstance("Noctara", 1, 0, "Storage", &storageUi);
    qmlRegisterSingletonInstance("Noctara", 1, 0, "Catalog", &catalog);
    qmlRegisterSingletonInstance("Noctara", 1, 0, "Account", &account);
    qmlRegisterSingletonInstance("Noctara", 1, 0, "Player", &player);
    qmlRegisterSingletonInstance("Noctara", 1, 0, "Themes", &themesUi);
    qmlRegisterSingletonInstance("Noctara", 1, 0, "Shortcuts", &keyboard);
    qmlRegisterSingletonInstance("Noctara", 1, 0, "Layout", &layout);
    qmlRegisterSingletonInstance("Noctara", 1, 0, "Tray", &tray);
    qmlRegisterUncreatableType<models::PlaylistDetail>("Noctara", 1, 0, "PlaylistDetail",
                                                       QStringLiteral("Provided by Catalog"));
    qmlRegisterUncreatableType<models::UserDetail>("Noctara", 1, 0, "UserDetail",
                                                   QStringLiteral("Provided by Catalog"));
    qmlRegisterUncreatableType<media::QueueModel>("Noctara", 1, 0, "QueueModel", QStringLiteral("Provided by Player"));
    qmlRegisterType<ui::WaveformItem>("Noctara", 1, 0, "WaveformItem");
    qmlRegisterUncreatableType<models::CollectionModel>("Noctara", 1, 0, "CollectionModel",
                                                        QStringLiteral("Provided by Catalog"));
    qmlRegisterUncreatableType<ui::SettingsModel>("Noctara", 1, 0, "SettingsModel",
                                                  QStringLiteral("Provided by SettingsUi"));
    // Registered as a type as well so QML can use its State enum (AccountController.SignedIn); the instance is
    // the Account singleton above.
    qmlRegisterUncreatableType<ui::AccountController>("Noctara", 1, 0, "AccountController",
                                                      QStringLiteral("Provided by Account"));
    QObject::connect(&languages, &i18n::LanguageManager::languageChanged, &engine, &QQmlEngine::retranslate);

    // Scripted runs (smoke tests, screenshots) start the same way every time.
    const bool scripted = cli.smokeTest || !cli.screenshot.isEmpty();
    {
        // "settings/appearance" opens a settings category directly.
        const QString start = cli.page.isEmpty() ? (scripted ? QStringLiteral("home") : layout.startPage()) : cli.page;
        QVariantMap initial{{QStringLiteral("page"), start.section(u'/', 0, 0)}};
        if (start.contains(u'/')) {
            initial.insert(QStringLiteral("settingsCategory"), start.section(u'/', 1, 1));
        }
        engine.setInitialProperties(initial);
    }
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    if (auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst())) {
        mainWindow = window;
        graphics.watch(window);
        // Main.qml starts hidden so GPU timestamps can be enabled before the scene graph initialises.
        gfx::configureWindow(*window, decision, paths.cacheDir);
        frames.attach(window);
        if (!scripted) {
            layout.restore(*window);
        }
        if (!scripted && layout.startMinimized()) {
            // In the tray when there is one (the window stays hidden), otherwise minimized.
            if (!tray.active()) {
                window->showMinimized();
            }
        } else {
            window->setVisible(true);
        }
        QObject::connect(app.get(), &QCoreApplication::aboutToQuit, window, [window, &layout, scripted] {
            if (!scripted) {
                layout.remember(*window, window->property("page").toString());
            }
        });
        // --navigate a,b,c switches every 500 ms (exercises transitions and returning to earlier pages).
        const QStringList steps = cli.navigate.split(u',', Qt::SkipEmptyParts);
        for (qsizetype i = 0; i < steps.size(); ++i) {
            QTimer::singleShot(600 + static_cast<int>(i) * 500, window, [window, target = steps.at(i)] {
                if (target == u"appearance-panel") {
                    window->setProperty("appearancePanel", true);
                    return;
                }
                if (target == u"queue-panel") {
                    window->setProperty("queuePanel", true);
                    return;
                }
                if (target == u"full-player" || target == u"cheat-sheet") {
                    window->setProperty(target == u"full-player" ? "fullPlayer" : "cheatSheet", true);
                    return;
                }
                window->setProperty("page", target.section(u'/', 0, 0));
                if (target.contains(u'/')) {
                    window->setProperty("settingsCategory", target.section(u'/', 1, 1));
                }
            });
        }
        if (!cli.screenshot.isEmpty()) {
            const int delay = steps.isEmpty() ? 1500 : 600 + static_cast<int>(steps.size() - 1) * 500 + 900;
            QTimer::singleShot(delay, window, [window, path = cli.screenshot] {
                if (!window->grabWindow().save(path)) {
                    qCWarning(lcUi) << "could not save screenshot to" << path;
                }
                QCoreApplication::quit();
            });
        }
        // frameSwapped comes from the render thread; disarm on the GUI thread after the first frame.
        QObject::connect(
            window, &QQuickWindow::frameSwapped, &graphics, [&sentinel] { sentinel.disarm(); },
            static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::SingleShotConnection));
    }
    QTimer::singleShot(0, &appController, [&appController, issues] { appController.reportConfigIssues(issues); });
    if (!cli.artwork.isEmpty()) {
        const QImage image(cli.artwork);
        if (image.isNull()) {
            qCWarning(lcUi) << "could not load artwork" << cli.artwork;
        }
        accent.setArtwork(cli.artwork, image);
    }
    if (cli.smokeTest) {
        runSmokeTest(engine, settingsUi);
    }
    if (!cli.play.isEmpty()) {
        (void)api::fetch(soundcloud.client(), api::endpoints::resolve(QUrl(cli.play)), &player,
                         [&player, &appController](Result<api::Item> item) {
                             if (item) {
                                 player.playItem(*item);
                             } else {
                                 appController.showToast(QStringLiteral("error"),
                                                         i18n::errorMessage(item.error().code));
                             }
                         });
    }
    if (cli.pgoScenario) {
        QTimer::singleShot(5000, app.get(), &QCoreApplication::quit);
    }

    const int code = QCoreApplication::exec();
    config.flush();
    return code;
}
