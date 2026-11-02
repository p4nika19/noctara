#include "ui/EffectsController.hpp"

#include "core/Log.hpp"
#include "gfx/FrameMonitor.hpp"
#include "gfx/GraphicsBootstrap.hpp"
#include "gfx/PowerMonitor.hpp"
#include "i18n/LanguageManager.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"
#include "theme/Theme.hpp"
#include "ui/SettingsController.hpp"

#include <QCoreApplication>

namespace noctara::ui {

namespace {

namespace keys = settings::keys;

QString keyName(std::string_view key)
{
    return QString::fromLatin1(key.data(), static_cast<qsizetype>(key.size()));
}

QString styleName(const QString& style)
{
    if (style == u"kawase") {
        return QCoreApplication::translate("Effects", "Dual Kawase blur");
    }
    if (style == u"gaussian") {
        return QCoreApplication::translate("Effects", "Gaussian blur");
    }
    return QCoreApplication::translate("Effects", "no blur");
}

QString qualityName(int quality)
{
    switch (quality) {
    case 0: return QCoreApplication::translate("Effects", "Low");
    case 1: return QCoreApplication::translate("Effects", "Medium");
    default: return QCoreApplication::translate("Effects", "High");
    }
}

}  // namespace

EffectsController::EffectsController(settings::SettingsStore& settings, theme::Theme& theme, gfx::PowerMonitor& power,
                                     gfx::FrameMonitor& frames, const gfx::GraphicsInfo& graphics,
                                     i18n::LanguageManager& languages, SettingsController& settingsUi,
                                     gfx::GovernorConfig governor, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_theme(theme)
    , m_power(power)
    , m_frames(frames)
    , m_graphics(graphics)
    , m_settingsUi(settingsUi)
    , m_governor(governor)
{
    m_clock.start();
    connect(&m_frames, &gfx::FrameMonitor::measured, this, &EffectsController::onMeasured);
    connect(&m_power, &gfx::PowerMonitor::stateChanged, this, &EffectsController::update);
    connect(&languages, &i18n::LanguageManager::languageChanged, this, &EffectsController::updateRowStatuses);
    connect(&m_theme, &theme::Theme::glassChanged, this, &EffectsController::updateRowStatuses);
    connect(&m_settings, &settings::SettingsStore::valueChanged, this, [this](const QString& key) {
        if (key == keyName(keys::GlassQuality)) {
            m_governor.reset();  // a manual choice starts over; Auto measures again from High
            update();
        } else if (key == keyName(keys::EnergySaving) || key == keyName(keys::FrameRateCap)) {
            update();
        }
    });
    update();
}

void EffectsController::resetGovernor()
{
    m_governor.reset();
    update();
}

void EffectsController::onMeasured(const gfx::FrameWindowStats& stats, double fps)
{
    m_fps = fps;
    m_gpuFrameMs = stats.gpuP90Ms.value_or(-1.0);
    m_budgetMs = stats.budgetMs;
    Q_EMIT statsChanged();
    // QT_LOGGING_RULES="noctara.gfx.debug=true" prints these once per second of rendering (docs/performance.md).
    qCDebug(lcGfx).noquote() << "frames:" << stats.frames << "fps" << fps << "gpu p90" << m_gpuFrameMs
                             << "ms, interval p90" << stats.intervalP90Ms << "ms, style" << m_theme.glass().style
                             << "quality" << m_theme.glass().quality;

    // At Solid the governor's own cap removed the blur: keep measuring, so headroom can bring it back.
    const auto& glass = m_theme.glass();
    const bool blurWanted =
        glass.blurred || (m_governor.tier() == gfx::Tier::Solid && glass.blurAvailable &&
                          glass.requestedStyle != u"solid" && !m_settings.boolValue(keys::ReduceTransparency));
    if (!m_graphics.hardware() || !blurWanted ||
        gfx::parseQualitySetting(m_settings.stringValue(keys::GlassQuality)) != gfx::QualitySetting::Auto) {
        return;  // nothing to govern: no blur, or quality fixed by the user
    }
    if (const auto tier = m_governor.feed(stats, static_cast<double>(m_clock.elapsed()) / 1000.0)) {
        qCInfo(lcGfx).noquote() << "quality governor:" << gfx::tierName(*tier) << "(gpu p90" << m_gpuFrameMs
                                << "ms, interval p90" << stats.intervalP90Ms << "ms, budget" << stats.budgetMs << "ms)";
        update();
    }
}

void EffectsController::update()
{
    gfx::EffectsInputs inputs;
    inputs.quality = gfx::parseQualitySetting(m_settings.stringValue(keys::GlassQuality));
    inputs.governorTier = m_governor.tier();
    inputs.energySaving = gfx::energySavingActive(m_settings.stringValue(keys::EnergySaving), m_power.state());
    inputs.hardware = m_graphics.hardware();
    inputs.frameRateCap = m_settings.stringValue(keys::FrameRateCap);
    const gfx::EffectsState state = gfx::resolveEffects(inputs);

    m_theme.setGlassLimits(state.quality, state.styleCap, state.backdropAnimation);
    m_frames.setFrameCap(state.frameCapFps);
    if (!(state == m_state)) {
        m_state = state;
        Q_EMIT stateChanged();
    }
    updateRowStatuses();
}

void EffectsController::updateRowStatuses()
{
    const auto& glass = m_theme.glass();
    QString styleStatus;
    if (glass.style != glass.requestedStyle && m_graphics.hardware() &&
        !m_settings.boolValue(keys::ReduceTransparency) && glass.requestedStyle != u"solid") {
        styleStatus =
            m_state.energySaving
                ? QCoreApplication::translate("Effects", "Energy saving is active: %1 is used for now.")
                      .arg(styleName(glass.style))
                : QCoreApplication::translate("Effects", "Lowered automatically to %1 to keep animations smooth.")
                      .arg(styleName(glass.style));
    } else if (m_settings.boolValue(keys::ReduceTransparency) && glass.requestedStyle != u"solid") {
        styleStatus = QCoreApplication::translate("Effects", "Reduce transparency is on, so surfaces are opaque.");
    }
    m_settingsUi.setRowStatus(keyName(keys::GlassStyle), styleStatus);

    QString surfaceStatus;
    if (glass.surface != glass.requestedSurface) {
        if (!glass.blurred) {
            surfaceStatus = QCoreApplication::translate("Effects", "Liquid Glass needs a blur: frosted glass is used.");
        } else {
            surfaceStatus =
                m_state.energySaving
                    ? QCoreApplication::translate("Effects", "Energy saving is active: frosted glass is used for now.")
                    : QCoreApplication::translate("Effects",
                                                  "Switched to frosted glass automatically to keep animations smooth.");
        }
    }
    m_settingsUi.setRowStatus(keyName(keys::GlassSurface), surfaceStatus);

    QString qualityStatus;
    if (m_state.energySaving) {
        qualityStatus = QCoreApplication::translate("Effects", "Energy saving is active: effects run at %1 quality.")
                            .arg(qualityName(m_state.quality));
    } else if (m_state.governed) {
        qualityStatus =
            QCoreApplication::translate("Effects",
                                        "Lowered automatically to %1 because frames took too long. It is raised "
                                        "again when there is headroom.")
                .arg(m_governor.tier() == gfx::Tier::Solid ? styleName(glass.style) : qualityName(m_state.quality));
    }
    m_settingsUi.setRowStatus(keyName(keys::GlassQuality), qualityStatus);
}

}  // namespace noctara::ui
