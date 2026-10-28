#include "media/PcmRing.hpp"

#include <algorithm>
#include <cstring>

namespace noctara::media {

PcmRing::PcmRing(int channels, std::int64_t capacityFrames)
    : m_channels(std::max(channels, 1))
    , m_capacity(std::max<std::int64_t>(capacityFrames, 1))
    , m_data(static_cast<std::size_t>(m_capacity * m_channels))
{
}

bool PcmRing::write(std::span<const float> samples, const std::stop_token& stop)
{
    const auto channels = static_cast<std::size_t>(m_channels);
    std::size_t offset = 0;
    std::unique_lock lock(m_mutex);
    while (offset < samples.size()) {
        if (!m_space.wait(lock, stop, [&] { return m_size < m_capacity; })) {
            return false;
        }
        const auto frames = static_cast<std::int64_t>((samples.size() - offset) / channels);
        const std::int64_t tail = (m_head + m_size) % m_capacity;
        const std::int64_t chunk = std::min({frames, m_capacity - m_size, m_capacity - tail});
        if (chunk <= 0) {
            break;
        }
        std::memcpy(m_data.data() + (static_cast<std::size_t>(tail) * channels), samples.data() + offset,
                    static_cast<std::size_t>(chunk) * channels * sizeof(float));
        m_size += chunk;
        offset += static_cast<std::size_t>(chunk) * channels;
    }
    return true;
}

void PcmRing::finish()
{
    const std::scoped_lock lock(m_mutex);
    m_finished = true;
}

void PcmRing::reset(std::int64_t frame)
{
    {
        const std::scoped_lock lock(m_mutex);
        m_head = 0;
        m_size = 0;
        m_position = frame;
        m_origin = frame;
        m_finished = false;
        m_held = false;
    }
    m_space.notify_all();
}

void PcmRing::hold()
{
    const std::scoped_lock lock(m_mutex);
    m_held = true;
}

std::int64_t PcmRing::read(std::span<float> out)
{
    const auto channels = static_cast<std::size_t>(m_channels);
    std::int64_t copied = 0;
    {
        const std::scoped_lock lock(m_mutex);
        if (m_held) {
            return 0;
        }
        const std::int64_t wanted = std::min(static_cast<std::int64_t>(out.size() / channels), m_size);
        while (copied < wanted) {
            const std::int64_t chunk = std::min(wanted - copied, m_capacity - m_head);
            std::memcpy(out.data() + (static_cast<std::size_t>(copied) * channels),
                        m_data.data() + (static_cast<std::size_t>(m_head) * channels),
                        static_cast<std::size_t>(chunk) * channels * sizeof(float));
            m_head = (m_head + chunk) % m_capacity;
            copied += chunk;
        }
        m_size -= copied;
        m_position += copied;
    }
    if (copied > 0) {
        m_space.notify_one();
    }
    return copied;
}

std::int64_t PcmRing::available() const
{
    const std::scoped_lock lock(m_mutex);
    return m_held ? 0 : m_size;
}

bool PcmRing::finished() const
{
    const std::scoped_lock lock(m_mutex);
    return m_finished;
}

bool PcmRing::drained() const
{
    const std::scoped_lock lock(m_mutex);
    return m_finished && m_size == 0 && !m_held;
}

bool PcmRing::held() const
{
    const std::scoped_lock lock(m_mutex);
    return m_held;
}

std::int64_t PcmRing::position() const
{
    const std::scoped_lock lock(m_mutex);
    return m_position;
}

std::int64_t PcmRing::origin() const
{
    const std::scoped_lock lock(m_mutex);
    return m_origin;
}

}  // namespace noctara::media
