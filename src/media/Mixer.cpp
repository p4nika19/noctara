#include "media/Mixer.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace noctara::media {

namespace {

constexpr std::int64_t kBlock = 1024;

float approach(float value, float target, float step)
{
    return value < target ? std::min(value + step, target) : std::max(value - step, target);
}

}  // namespace

Mixer::Mixer(AudioFormat format)
    : m_format(format)
    , m_rampFrames(std::max<std::int64_t>(1, static_cast<std::int64_t>(format.sampleRate) * kRampMs / 1000))
    , m_a(static_cast<std::size_t>(kBlock * format.channels))
    , m_b(static_cast<std::size_t>(kBlock * format.channels))
{
}

void Mixer::setVoices(std::shared_ptr<PcmRing> current, std::shared_ptr<PcmRing> next)
{
    const std::scoped_lock lock(m_mutex);
    m_current = std::move(current);
    m_next = std::move(next);
    m_fadeLength = 0;
    m_fadeIn = 0.0F;
    m_ended = false;
}

bool Mixer::replaceNext(std::shared_ptr<PcmRing> ring, std::uint64_t seenAdvances)
{
    const std::scoped_lock lock(m_mutex);
    if (m_advances.load() != seenAdvances) {
        return false;
    }
    if (m_fadeLength > 0 && ring != m_next) {
        m_fadeLength = 0;  // the crossfade target went away; the current voice finishes alone
    }
    m_next = std::move(ring);
    if (m_next) {
        m_ended = false;
    }
    return true;
}

void Mixer::interrupt()
{
    const std::scoped_lock lock(m_mutex);
    m_fadeLength = 0;
    m_ended = false;
}

void Mixer::setCrossfadeFrames(std::int64_t frames)
{
    const std::scoped_lock lock(m_mutex);
    m_crossfadeFrames = std::max<std::int64_t>(frames, 0);
}

void Mixer::setGain(float gain)
{
    const std::scoped_lock lock(m_mutex);
    m_targetGain = std::clamp(gain, 0.0F, 1.0F);
}

void Mixer::setPaused(bool paused)
{
    const std::scoped_lock lock(m_mutex);
    m_paused = paused;
    if (!paused) {
        m_silent = false;
    }
}

void Mixer::render(std::span<float> out)
{
    std::ranges::fill(out, 0.0F);
    const auto channels = static_cast<std::size_t>(m_format.channels);
    const auto frames = static_cast<std::int64_t>(out.size() / channels);
    const std::scoped_lock lock(m_mutex);
    for (std::int64_t done = 0; done < frames; done += kBlock) {
        renderBlock(out.data() + (static_cast<std::size_t>(done) * channels), std::min(kBlock, frames - done));
    }
}

void Mixer::renderBlock(float* out, std::int64_t frames)
{
    if (m_paused && m_pauseGain <= 0.0F) {
        m_silent = true;
        return;  // nothing is consumed while paused
    }
    const std::int64_t got = pull(out, frames);
    const float step = 1.0F / static_cast<float>(m_rampFrames);
    const auto channels = static_cast<std::size_t>(m_format.channels);
    for (std::int64_t i = 0; i < got; ++i) {
        m_gain = approach(m_gain, m_targetGain, step);
        m_pauseGain = approach(m_pauseGain, m_paused ? 0.0F : 1.0F, step);
        m_fadeIn = approach(m_fadeIn, 1.0F, step);
        const float gain = m_gain * m_pauseGain * m_fadeIn;
        float* frame = out + (static_cast<std::size_t>(i) * channels);
        for (std::size_t c = 0; c < channels; ++c) {
            frame[c] *= gain;
        }
    }
    if (got < frames) {
        m_fadeIn = 0.0F;  // whatever comes after the gap fades in
        if (m_paused) {
            m_pauseGain = 0.0F;
        }
    }
}

std::int64_t Mixer::pull(float* out, std::int64_t frames)
{
    const auto channels = static_cast<std::size_t>(m_format.channels);
    const auto at = [&](std::int64_t frame) {
        return out + (static_cast<std::size_t>(frame) * channels);
    };
    std::int64_t done = 0;
    bool starved = false;
    while (done < frames && m_current) {
        if (m_fadeLength == 0 && m_next && m_crossfadeFrames > 0 && m_current->finished()) {
            const std::int64_t left = m_current->available();
            if (left > 0 && left <= m_crossfadeFrames) {
                m_fadeLength = left;
            }
        }
        if (m_fadeLength > 0) {
            const std::int64_t left = m_current->available();
            const std::int64_t a = m_current->read(
                std::span(at(done), static_cast<std::size_t>(std::min(frames - done, left)) * channels));
            std::fill_n(m_b.begin(), static_cast<std::ptrdiff_t>(a) * m_format.channels, 0.0F);
            (void)m_next->read(std::span(m_b.data(), static_cast<std::size_t>(a) * channels));
            const auto length = static_cast<double>(m_fadeLength);
            for (std::int64_t i = 0; i < a; ++i) {
                const double progress = (length - static_cast<double>(left - i)) / length;
                const auto fadeOut = static_cast<float>(std::cos(progress * std::numbers::pi / 2));
                const auto fadeIn = static_cast<float>(std::sin(progress * std::numbers::pi / 2));
                float* frame = at(done + i);
                const float* incoming = m_b.data() + (static_cast<std::size_t>(i) * channels);
                for (std::size_t c = 0; c < channels; ++c) {
                    frame[c] = (frame[c] * fadeOut) + (incoming[c] * fadeIn);
                }
            }
            done += a;
            if (m_current->drained()) {
                promote();
            } else if (a == 0) {
                starved = true;
                break;
            }
            continue;
        }
        std::int64_t want = frames - done;
        if (m_next && m_crossfadeFrames > 0 && m_current->finished()) {
            // Stop where the crossfade begins; the next pass blends.
            const std::int64_t beforeFade = m_current->available() - m_crossfadeFrames;
            if (beforeFade > 0 && beforeFade < want) {
                done += m_current->read(std::span(at(done), static_cast<std::size_t>(beforeFade) * channels));
                continue;
            }
        }
        done += m_current->read(std::span(at(done), static_cast<std::size_t>(want) * channels));
        if (done < frames) {
            if (!m_current->drained()) {
                starved = true;
                break;
            }
            if (!m_next) {
                m_ended = true;
                break;
            }
            promote();
        }
    }
    m_starved = starved;
    return done;
}

void Mixer::promote()
{
    m_current = std::move(m_next);
    m_next.reset();
    m_fadeLength = 0;
    m_ended = false;
    m_advances.fetch_add(1);
}

}  // namespace noctara::media
