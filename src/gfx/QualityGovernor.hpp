#pragma once

#include <QString>

#include <cstdint>
#include <optional>

// Adaptive effects quality. Pure logic: fed with per-second frame statistics, it decides when to lower or
// raise the quality of the glass effects. No Qt GUI dependency, unit-tested with synthetic timings.
namespace noctara::gfx {

// Quality tiers in the order the governor steps down: blur resolution first, then the number of blur passes,
// then the glass style (Liquid Glass and Gaussian become Dual Kawase), and finally Solid.
enum class Tier : std::uint8_t {
    Solid,   // no blur at all
    Kawase,  // low quality, glass style capped at Dual Kawase
    Low,     // quarter-resolution capture, one pass fewer
    Medium,  // third-resolution capture
    High,    // half-resolution capture
};

[[nodiscard]] QString tierName(Tier tier);

// One measurement window (about a second of rendering).
struct FrameWindowStats {
    int frames = 0;                   // frames rendered during the window
    double budgetMs = 1000.0 / 60.0;  // time available per frame at the current refresh rate or cap
    std::optional<double> gpuP90Ms;   // GPU time per frame (90th percentile), when timestamps are available
    double intervalP90Ms = 0.0;       // time between consecutive frames while rendering continuously
};

struct GovernorConfig {
    int minFrames = 30;             // windows with fewer frames (idle UI) are not judged
    double gpuOverload = 0.85;      // GPU p90 above this share of the budget is an overload
    double gpuHeadroom = 0.45;      // ... below this share leaves room for more quality
    double intervalOverload = 1.5;  // p90 frame interval above this many budgets means dropped frames
    double intervalHealthy = 1.15;  // ... below this many budgets is smooth
    int overloadWindows = 2;        // consecutive overloaded windows before stepping down
    int healthyWindows = 5;         // consecutive healthy windows before stepping up
    double upCooldownS = 20.0;      // minimum time since the last change before stepping up
    double maxCooldownS = 600.0;    // the cooldown doubles on oscillation, up to this
    double oscillationS = 30.0;     // a step down this soon after a step up counts as oscillation
};

class QualityGovernor {
public:
    explicit QualityGovernor(GovernorConfig config = {}) noexcept;

    // Judges one window; returns the new tier when it changes.
    std::optional<Tier> feed(const FrameWindowStats& stats, double nowSeconds) noexcept;
    // Back to High with the initial cooldown (user reset, or the quality setting changed).
    void reset() noexcept;

    [[nodiscard]] Tier tier() const noexcept { return m_tier; }
    [[nodiscard]] double cooldownSeconds() const noexcept { return m_cooldown; }

private:
    enum class Verdict : std::uint8_t { Unknown, Overloaded, Healthy, Neutral };
    [[nodiscard]] Verdict judge(const FrameWindowStats& stats) const noexcept;

    GovernorConfig m_config;
    Tier m_tier = Tier::High;
    double m_cooldown;
    double m_lastChange = -1e9;
    double m_lastStepUp = -1e9;
    int m_overloaded = 0;
    int m_healthy = 0;
};

enum class QualitySetting : std::uint8_t { Auto, Low, Medium, High };
[[nodiscard]] QualitySetting parseQualitySetting(const QString& text) noexcept;

struct EffectsInputs {
    QualitySetting quality = QualitySetting::Auto;
    Tier governorTier = Tier::High;
    bool energySaving = false;
    bool hardware = true;
    QString frameRateCap = QStringLiteral("auto");  // performance.rendering.frame_rate_cap
};

// What the effects may cost right now.
struct EffectsState {
    int quality = 2;   // blur quality: 0 low, 1 medium, 2 high
    QString styleCap;  // most expensive glass style allowed: "" (any), "kawase" or "solid"
    bool backdropAnimation = true;
    int frameCapFps = 0;        // 0 = no cap beyond VSync
    bool governed = false;      // the governor currently lowers the effects (Auto quality)
    bool energySaving = false;  // energy saving currently lowers the effects

    friend bool operator==(const EffectsState&, const EffectsState&) = default;
};

[[nodiscard]] EffectsState resolveEffects(const EffectsInputs& inputs);

}  // namespace noctara::gfx
