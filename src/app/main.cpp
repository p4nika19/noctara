#include "core/Log.hpp"
#include "core/LsanSuppressions.hpp"
#include "core/Paths.hpp"
#include "gfx/CrashSentinel.hpp"
#include "gfx/GraphicsBootstrap.hpp"
#include "gfx/PowerMonitor.hpp"
#include "gfx/Relaunch.hpp"
#include "i18n/Formatter.hpp"
#include "i18n/LanguageManager.hpp"
#include "settings/ConfigFile.hpp"
#include "settings/Overrides.hpp"
#include "settings/SchemaDoc.hpp"
#include "settings/SettingsStore.hpp"
#include "theme/FontLoader.hpp"
#include "theme/Theme.hpp"
#include "ui/AppController.hpp"
#include "ui/IconProvider.hpp"
#include "ui/PowerStatus.hpp"
#include "ui/SettingsController.hpp"
#include "ui/SettingsModel.hpp"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QProcessEnvironment>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTimer>

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
    QString navigate;        // dev/testing: page to switch to after startup (exercises transitions)
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
    parser.addOptions({config, print, reset, safe, disableGpu, set, pgo, screenshot, page, smoke, navigate});

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

ui::Availability graphicsAvailability(const gfx::GraphicsDecision& decision)
{
    ui::Availability availability;
    if (!decision.hardware) {
        static const char* const reason =
            QT_TRANSLATE_NOOP("Settings", "Needs hardware acceleration, which is currently off.");
        auto& glass = availability.disabledOptions[QStringLiteral("appearance.glass.style")];
        glass.insert(QStringLiteral("kawase"), reason);
        glass.insert(QStringLiteral("gaussian"), reason);
        glass.insert(QStringLiteral("liquid"), reason);
        availability.disabledSettings.insert(QStringLiteral("appearance.glass.blur_strength"), reason);
        availability.disabledSettings.insert(QStringLiteral("appearance.backdrop.animate"), reason);
    } else if (!decision.liquidGlassAllowed) {
        availability.disabledOptions[QStringLiteral("appearance.glass.style")].insert(
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
    log::install();
    QCoreApplication::setApplicationName(QString::fromLatin1(kAppId));
    QCoreApplication::setApplicationVersion(QStringLiteral(NOCTARA_VERSION));
    QGuiApplication::setDesktopFileName(QString::fromLatin1(kAppId));

    const std::span<char*> rawArgs(argv, static_cast<std::size_t>(argc));
    const bool headless = hasFlag(rawArgs, "--print-config") || hasFlag(rawArgs, "--reset-config") ||
                          hasFlag(rawArgs, "--help") || hasFlag(rawArgs, "-h") || hasFlag(rawArgs, "--version");
    const std::unique_ptr<QCoreApplication> app =
        headless ? std::make_unique<QCoreApplication>(argc, argv) : std::make_unique<QGuiApplication>(argc, argv);

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

    ui::SettingsController settingsUi(store, config, languages);
    settingsUi.setAvailability(graphicsAvailability(decision));
    gfx::PowerMonitor power;
    const ui::PowerStatusPresenter powerStatus(power, store, languages, settingsUi);
    ui::AppController appController(store, graphics, relaunch, languages);
    QObject::connect(&config, &settings::ConfigFile::reloadIssues, &appController,
                     &ui::AppController::reportConfigIssues);
    config.setHotReloadEnabled(true);

    QQmlApplicationEngine engine;
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): QQmlEngine takes ownership of image providers
    engine.addImageProvider(QStringLiteral("icon"), new ui::IconProvider);
    qmlRegisterSingletonInstance("Noctara", 1, 0, "Theme", &theme);
    qmlRegisterSingletonInstance("Noctara", 1, 0, "Format", &formatter);
    qmlRegisterSingletonInstance("Noctara", 1, 0, "SettingsUi", &settingsUi);
    qmlRegisterSingletonInstance("Noctara", 1, 0, "App", &appController);
    qmlRegisterSingletonInstance("Noctara", 1, 0, "Graphics", &graphics);
    qmlRegisterSingletonInstance("Noctara", 1, 0, "Language", &languages);
    qmlRegisterUncreatableType<ui::SettingsModel>("Noctara", 1, 0, "SettingsModel",
                                                  QStringLiteral("Provided by SettingsUi"));
    QObject::connect(&languages, &i18n::LanguageManager::languageChanged, &engine, &QQmlEngine::retranslate);

    if (!cli.page.isEmpty()) {
        // "settings/appearance" opens a settings category directly.
        QVariantMap initial{{QStringLiteral("page"), cli.page.section(u'/', 0, 0)}};
        if (cli.page.contains(u'/')) {
            initial.insert(QStringLiteral("settingsCategory"), cli.page.section(u'/', 1, 1));
        }
        engine.setInitialProperties(initial);
    }
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    if (auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst())) {
        graphics.watch(window);
        if (!cli.navigate.isEmpty()) {
            QTimer::singleShot(600, window, [window, target = cli.navigate] {
                window->setProperty("page", target.section(u'/', 0, 0));
                if (target.contains(u'/')) {
                    window->setProperty("settingsCategory", target.section(u'/', 1, 1));
                }
            });
        }
        if (!cli.screenshot.isEmpty()) {
            QTimer::singleShot(1500, window, [window, path = cli.screenshot] {
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
    if (cli.smokeTest) {
        runSmokeTest(engine, settingsUi);
    }
    if (cli.pgoScenario) {
        QTimer::singleShot(5000, app.get(), &QCoreApplication::quit);
    }

    const int code = QCoreApplication::exec();
    config.flush();
    return code;
}
