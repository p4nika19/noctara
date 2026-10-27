#include "ui/AppController.hpp"

#include "core/FileIo.hpp"
#include "core/Log.hpp"
#include "gfx/GraphicsBootstrap.hpp"
#include "gfx/Relaunch.hpp"
#include "i18n/ErrorMessages.hpp"
#include "i18n/LanguageManager.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"

#include <QCoreApplication>
#include <QHash>

namespace noctara::ui {

namespace {

constexpr int kMaxConfigToasts = 3;

}  // namespace

AppController::AppController(settings::SettingsStore& settings, gfx::GraphicsInfo& graphics,
                             gfx::RelaunchGuard& relaunch, i18n::LanguageManager& languages, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_graphics(graphics)
    , m_relaunch(relaunch)
{
    connect(&languages, &i18n::LanguageManager::languageChanged, this, &AppController::graphicsNoticeChanged);
    connect(&m_graphics, &gfx::GraphicsInfo::noticePendingChanged, this, &AppController::graphicsNoticeChanged);
    connect(&m_graphics, &gfx::GraphicsInfo::graphicsFailed, this, &AppController::onGraphicsFailed);
}

QString AppController::version() const
{
    return QStringLiteral(NOCTARA_VERSION);
}

QString AppController::graphicsNotice() const
{
    if (!m_graphics.noticePending()) {
        return {};
    }
    using gfx::Reason;
    switch (static_cast<Reason>(m_graphics.reason())) {
    case Reason::SafeModeFlag:
        return tr("Safe Mode: hardware acceleration and glass effects are off for this session. "
                  "Start Noctara without --safe-mode to return to normal.");
    case Reason::CrashRecovery:
        return tr("Noctara started in Safe Mode because the previous launch crashed while initialising graphics. "
                  "Restart to try hardware acceleration again, or turn it off in Settings → Performance.");
    case Reason::ProbeFailed:
        return tr("No usable graphics driver was found, so Noctara is drawing on the CPU. Blur effects are disabled.");
    case Reason::SoftwareRasterizer:
        return tr("Your system only provides software rendering, so Noctara is drawing on the CPU. "
                  "Blur effects are disabled.");
    case Reason::Blocklisted:
        return tr("Your graphics driver is known to cause problems, so hardware acceleration is off. "
                  "You can force it on in Settings → Performance.");
    case Reason::ApiFallback: return tr("The selected graphics API could not be started; another one is used instead.");
    case Reason::Hardware:
    case Reason::UserDisabled: break;
    }
    return {};
}

void AppController::dismissGraphicsNotice()
{
    m_graphics.acknowledgeNotice();
}

QString AppController::licenseText(const QString& id) const
{
    static const QHash<QString, QString> files{
        {QStringLiteral("manrope"), QStringLiteral(":/fonts/licenses/Manrope-OFL.txt")},
        {QStringLiteral("inter"), QStringLiteral(":/fonts/licenses/Inter-OFL.txt")},
        {QStringLiteral("jetbrains-mono"), QStringLiteral(":/fonts/licenses/JetBrainsMono-OFL.txt")},
        {QStringLiteral("lucide"), QStringLiteral(":/icons/LICENSE-lucide.txt")},
    };
    const auto it = files.constFind(id);
    if (it == files.constEnd()) {
        return {};
    }
    auto content = readWholeFile(*it);
    return content ? QString::fromUtf8(*content) : QString();
}

void AppController::restart()
{
    m_relaunch.request(QCoreApplication::applicationFilePath(), QCoreApplication::arguments().mid(1));
    QCoreApplication::quit();
}

void AppController::triggerAction(const QString& actionId)
{
    if (actionId == u"restart") {
        restart();
    }
}

void AppController::showToast(const QString& kind, const QString& message, const QString& actionLabel,
                              const QString& actionId)
{
    Q_EMIT toastRequested(kind, message, actionLabel, actionId);
}

void AppController::reportConfigIssues(const settings::ConfigIssues& issues)
{
    int shown = 0;
    for (const settings::ConfigIssue& issue : issues) {
        if (issue.severity != settings::ConfigIssue::Severity::Error) {
            continue;
        }
        if (shown++ == kMaxConfigToasts) {
            showToast(QStringLiteral("warning"),
                      tr("More configuration problems were found. See the log for details."));
            break;
        }
        showToast(QStringLiteral("warning"), i18n::configIssueMessage(issue));
    }
}

void AppController::onGraphicsFailed()
{
    // Persist the fallback so the next launch starts in software mode; tell the user and offer a restart.
    const auto disabled = m_settings.setUserValue(settings::keys::GpuAcceleration, QStringLiteral("off"));
    if (!disabled) {
        qCWarning(lcGfx) << "could not switch hardware acceleration off:" << disabled.error().describe().c_str();
    }
    showToast(QStringLiteral("error"),
              tr("The graphics device stopped working. Hardware acceleration has been turned off; "
                 "restart Noctara to continue in software mode."),
              tr("Restart now"), QStringLiteral("restart"));
}

}  // namespace noctara::ui
