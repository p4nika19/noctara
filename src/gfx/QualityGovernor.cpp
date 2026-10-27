#include "gfx/QualityGovernor.hpp"

#include <algorithm>

namespace noctara::gfx {

namespace {

constexpr int kFrameCapWhileSaving = 30;

Tier lower(Tier tier) noexcept
{
    return tier == Tier::Solid ? Tier::Solid : static_cast<Tier>(static_cast<std::uint8_t>(tier) - 1U);
}

Tier higher(Tier tier) noexcept
{
    return tier == Tier::High ? Tier::High : static_cast<Tier>(static_cast<std::uint8_t>(tier) + 1U);
}

}  // namespace

QString tierName(Tier tier)
{
    switch (tier) {
    case Tier::Solid: return QStringLiteral("solid");
    case Tier::Kawase: return QStringLiteral("kawase");
    case Tier::Low: return QStringLiteral("low");
    case Tier::Medium: return QStringLiteral("medium");
    case Tier::High: return QStringLiteral("high");
    }
    return {};
}

QualityGovernor::QualityGovernor(GovernorConfig config) noexcept
    : m_config(config)
    , m_cooldown(config.upCooldownS)
{
}

QualityGovernor::Verdict QualityGovernor::judge(const FrameWindowStats& stats) const noexcept
{
    if (stats.frames < m_config.minFrames || stats.budgetMs <= 0.0) {
        return Verdict::Unknown;
    }
    if (stats.gpuP90Ms) {
        const double load = *stats.gpuP90Ms / stats.budgetMs;
        if (load > m_config.gpuOverload) {
            return Verdict::Overloaded;
        }
        return load < m_config.gpuHeadroom ? Verdict::Healthy : Verdict::Neutral;
    }
    const double intervals = stats.intervalP90Ms / stats.budgetMs;
    if (intervals > m_config.intervalOverload) {
        return Verdict::Overloaded;
    }
    return intervals < m_config.intervalHealthy ? Verdict::Healthy : Verdict::Neutral;
}

std::optional<Tier> QualityGovernor::feed(const FrameWindowStats& stats, double nowSeconds) noexcept
{
    switch (judge(stats)) {
    case Verdict::Unknown: return std::nullopt;  // idle: neither evidence of trouble nor of headroom
    case Verdict::Neutral:
        m_overloaded = 0;
        m_healthy = 0;
        return std::nullopt;
    case Verdict::Overloaded:
        m_healthy = 0;
        if (++m_overloaded < m_config.overloadWindows || m_tier == Tier::Solid) {
            return std::nullopt;
        }
        if (nowSeconds - m_lastStepUp < m_config.oscillationS) {
            m_cooldown = std::min(m_cooldown * 2.0, m_config.maxCooldownS);
        }
        m_overloaded = 0;
        m_tier = lower(m_tier);
        m_lastChange = nowSeconds;
        return m_tier;
    case Verdict::Healthy:
        m_overloaded = 0;
        if (++m_healthy < m_config.healthyWindows || m_tier == Tier::High || nowSeconds - m_lastChange < m_cooldown) {
            return std::nullopt;
        }
        m_healthy = 0;
        m_tier = higher(m_tier);
        m_lastChange = nowSeconds;
        m_lastStepUp = nowSeconds;
        return m_tier;
    }
    return std::nullopt;
}

void QualityGovernor::reset() noexcept
{
    m_tier = Tier::High;
    m_cooldown = m_config.upCooldownS;
    m_lastChange = -1e9;
    m_lastStepUp = -1e9;
    m_overloaded = 0;
    m_healthy = 0;
}

QualitySetting parseQualitySetting(const QString& text) noexcept
{
    if (text == u"low") {
        return QualitySetting::Low;
    }
    if (text == u"medium") {
        return QualitySetting::Medium;
    }
    if (text == u"high") {
        return QualitySetting::High;
    }
    return QualitySetting::Auto;
}

EffectsState resolveEffects(const EffectsInputs& inputs)
{
    EffectsState state;
    switch (inputs.quality) {
    case QualitySetting::Low: state.quality = 0; break;
    case QualitySetting::Medium: state.quality = 1; break;
    case QualitySetting::High: state.quality = 2; break;
    case QualitySetting::Auto:
        state.governed = inputs.hardware && inputs.governorTier != Tier::High;
        switch (inputs.hardware ? inputs.governorTier : Tier::High) {
        case Tier::High: state.quality = 2; break;
        case Tier::Medium: state.quality = 1; break;
        case Tier::Low: state.quality = 0; break;
        case Tier::Kawase:
            state.quality = 0;
            state.styleCap = QStringLiteral("kawase");
            break;
        case Tier::Solid:
            state.quality = 0;
            state.styleCap = QStringLiteral("solid");
            break;
        }
        break;
    }

    bool ok = false;
    const int cap = inputs.frameRateCap.toInt(&ok);
    state.frameCapFps = ok && cap > 0 ? cap : 0;

    if (inputs.energySaving) {
        state.energySaving = true;
        state.quality = std::min(state.quality, 1);
        if (state.styleCap.isEmpty()) {
            state.styleCap = QStringLiteral("kawase");
        }
        state.backdropAnimation = false;
        state.frameCapFps =
            state.frameCapFps == 0 ? kFrameCapWhileSaving : std::min(state.frameCapFps, kFrameCapWhileSaving);
    }
    return state;
}

}  // namespace noctara::gfx
