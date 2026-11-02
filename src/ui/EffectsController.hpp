#pragma once

#include "gfx/QualityGovernor.hpp"

#include <QElapsedTimer>
#include <QObject>
#include <QString>

namespace noctara::settings {
class SettingsStore;
}
namespace noctara::theme {
class Theme;
}
namespace noctara::gfx {
class FrameMonitor;
class GraphicsInfo;
class PowerMonitor;
}  // namespace noctara::gfx
namespace noctara::i18n {
class LanguageManager;
}

namespace noctara::ui {

class SettingsController;

// QML "Performance" singleton. Decides what the glass effects may cost right now, from the "Effects quality"
// setting, the quality governor (fed by FrameMonitor), energy saving and the renderer, and applies it to the
// Theme (quality, style cap, backdrop animation) and to the frame pacer. Exposes live frame statistics and
// the reasons for any reduction to the Performance page.
class EffectsController : public QObject {
    Q_OBJECT
    Q_PROPERTY(double fps READ fps NOTIFY statsChanged)
    Q_PROPERTY(double gpuFrameMs READ gpuFrameMs NOTIFY statsChanged)  // 90th percentile; < 0 when unknown
    Q_PROPERTY(double frameBudgetMs READ frameBudgetMs NOTIFY statsChanged)
    Q_PROPERTY(QString tier READ tier NOTIFY stateChanged)  // governor tier: high | medium | low | kawase | solid
    Q_PROPERTY(bool governed READ governed NOTIFY stateChanged)
    Q_PROPERTY(bool energySaving READ energySaving NOTIFY stateChanged)
    Q_PROPERTY(int frameCap READ frameCap NOTIFY stateChanged)  // effective cap in FPS; 0 = none

public:
    EffectsController(settings::SettingsStore& settings, theme::Theme& theme, gfx::PowerMonitor& power,
                      gfx::FrameMonitor& frames, const gfx::GraphicsInfo& graphics, i18n::LanguageManager& languages,
                      SettingsController& settingsUi, gfx::GovernorConfig governor = {}, QObject* parent = nullptr);

    [[nodiscard]] double fps() const noexcept { return m_fps; }
    [[nodiscard]] double gpuFrameMs() const noexcept { return m_gpuFrameMs; }
    [[nodiscard]] double frameBudgetMs() const noexcept { return m_budgetMs; }
    [[nodiscard]] QString tier() const { return gfx::tierName(m_governor.tier()); }
    [[nodiscard]] bool governed() const noexcept { return m_state.governed; }
    [[nodiscard]] bool energySaving() const noexcept { return m_state.energySaving; }
    [[nodiscard]] int frameCap() const noexcept { return m_state.frameCapFps; }

    // Undo the governor's reductions (it measures again from High).
    Q_INVOKABLE void resetGovernor();

Q_SIGNALS:
    void statsChanged();
    void stateChanged();

private:
    void onMeasured(const gfx::FrameWindowStats& stats, double fps);
    void update();
    void updateRowStatuses();

    settings::SettingsStore& m_settings;
    theme::Theme& m_theme;
    gfx::PowerMonitor& m_power;
    gfx::FrameMonitor& m_frames;
    const gfx::GraphicsInfo& m_graphics;
    SettingsController& m_settingsUi;
    gfx::QualityGovernor m_governor;
    gfx::EffectsState m_state;
    QElapsedTimer m_clock;
    double m_fps = 0.0;
    double m_gpuFrameMs = -1.0;
    double m_budgetMs = 1000.0 / 60.0;
};

}  // namespace noctara::ui
