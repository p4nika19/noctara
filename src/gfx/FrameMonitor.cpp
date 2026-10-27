#include "gfx/FrameMonitor.hpp"

#include <QCoreApplication>
#include <QQuickGraphicsConfiguration>
#include <QScreen>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <rhi/qrhi.h>
#include <thread>

namespace noctara::gfx {

namespace {

constexpr auto kWindow = std::chrono::seconds(1);
// Longer gaps mean the scene was idle in between, not that a frame was slow.
constexpr double kMaxIntervalMs = 100.0;
// Wake up this much before the paced deadline: the following VSync wait absorbs the difference.
constexpr auto kPacingSlack = std::chrono::microseconds(1500);

}  // namespace

double percentile90(std::vector<double> samples)
{
    if (samples.empty()) {
        return 0.0;
    }
    const auto index = static_cast<std::ptrdiff_t>((samples.size() - 1) * 9 / 10);
    std::ranges::nth_element(samples, samples.begin() + index);
    return samples[static_cast<std::size_t>(index)];
}

FrameMonitor::FrameMonitor(QObject* parent)
    : QObject(parent)
{
}

void FrameMonitor::attach(QQuickWindow* window)
{
    m_window = window;
    QQuickGraphicsConfiguration config = window->graphicsConfiguration();
    config.setTimestamps(true);
    window->setGraphicsConfiguration(config);
    connect(window, &QQuickWindow::afterRendering, this, &FrameMonitor::onRendered, Qt::DirectConnection);
    connect(window, &QQuickWindow::frameSwapped, this, &FrameMonitor::onSwapped, Qt::DirectConnection);
}

void FrameMonitor::setFrameCap(int fps) noexcept
{
    m_frameCapFps.store(std::max(0, fps), std::memory_order_relaxed);
}

void FrameMonitor::onRendered()
{
    QRhiSwapChain* swapChain = m_window ? m_window->swapChain() : nullptr;
    QRhiCommandBuffer* commands = swapChain != nullptr ? swapChain->currentFrameCommandBuffer() : nullptr;
    if (commands == nullptr) {
        return;
    }
    // GPU time of the last frame that has completed (a frame or two behind); 0 when unsupported.
    const double seconds = commands->lastCompletedGpuTime();
    if (seconds > 0.0) {
        const std::scoped_lock lock(m_mutex);
        m_gpuMs.push_back(seconds * 1000.0);
    }
}

void FrameMonitor::onSwapped()
{
    const Clock::time_point now = Clock::now();
    const bool opening = !m_windowOpen.exchange(true);
    {
        const std::scoped_lock lock(m_mutex);
        if (opening) {
            m_windowStart = now;  // the window starts with the first frame after a pause
        }
        ++m_frames;
        if (m_hasLastSwap) {
            const double interval = std::chrono::duration<double, std::milli>(now - m_lastSwap).count();
            if (interval < kMaxIntervalMs) {
                m_intervalsMs.push_back(interval);
            }
        }
    }
    if (opening) {
        QMetaObject::invokeMethod(
            this, [this] { QTimer::singleShot(kWindow, this, &FrameMonitor::summarise); }, Qt::QueuedConnection);
    }

    // Pace from the previous presentation: sleep until shortly before the next allowed frame, then let the
    // VSync wait absorb the rest.
    const int cap = m_frameCapFps.load(std::memory_order_relaxed);
    if (cap > 0 && m_hasLastSwap && QThread::currentThread() != QCoreApplication::instance()->thread()) {
        const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / cap));
        const Clock::time_point deadline = m_lastSwap + period - kPacingSlack;
        if (deadline > now) {
            std::this_thread::sleep_until(deadline);
        }
    }
    m_lastSwap = now;
    m_hasLastSwap = true;
}

void FrameMonitor::summarise()
{
    std::vector<double> gpu;
    std::vector<double> intervals;
    int frames = 0;
    double elapsed = 0.0;
    {
        const std::scoped_lock lock(m_mutex);
        gpu.swap(m_gpuMs);
        intervals.swap(m_intervalsMs);
        frames = std::exchange(m_frames, 0);
        const Clock::time_point now = Clock::now();
        elapsed = std::chrono::duration<double>(now - m_windowStart).count();
        m_windowStart = now;
    }

    double refresh = 60.0;
    if (m_window != nullptr && m_window->screen() != nullptr && m_window->screen()->refreshRate() > 1.0) {
        refresh = m_window->screen()->refreshRate();
    }
    const int cap = m_frameCapFps.load(std::memory_order_relaxed);
    const double rate = cap > 0 ? std::min(refresh, static_cast<double>(cap)) : refresh;

    FrameWindowStats stats;
    stats.frames = frames;
    stats.budgetMs = 1000.0 / rate;
    if (!gpu.empty()) {
        stats.gpuP90Ms = percentile90(std::move(gpu));
    }
    stats.intervalP90Ms = percentile90(std::move(intervals));
    Q_EMIT measured(stats, elapsed > 0.0 ? frames / elapsed : 0.0);

    if (frames == 0) {
        m_windowOpen.store(false);  // idle: the next frame re-arms the timer
        return;
    }
    QTimer::singleShot(kWindow, this, &FrameMonitor::summarise);
}

}  // namespace noctara::gfx
