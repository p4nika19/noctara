#include "gfx/FrameMonitor.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QQuickGraphicsConfiguration>
#include <QScreen>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <rhi/qrhi.h>

namespace noctara::gfx {

namespace {

constexpr auto kWindow = std::chrono::seconds(1);
// Longer gaps mean the scene was idle in between, not that a frame was slow.
constexpr double kMaxIntervalMs = 100.0;
// Frame intervals longer than this many frame budgets are pauses between low-rate redraws, not slow frames.
constexpr double kGapBudgets = 2.25;
// Wake up this much before the paced deadline: the following VSync wait absorbs the difference.
constexpr auto kPacingSlack = std::chrono::microseconds(1500);
// A frame this far behind the pacing grid follows an idle scene: the grid restarts from it.
constexpr auto kIdleGap = std::chrono::milliseconds(100);
// How long the interactive cap lasts after an input: long enough for the transition or scroll it started.
constexpr auto kInteractionHold = std::chrono::seconds(1);

bool isUserInput(QEvent::Type type) noexcept
{
    switch (type) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
    case QEvent::Wheel:
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd: return true;
    default: return false;
    }
}

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

double frameIntervalP90(std::vector<double> intervalsMs, double budgetMs)
{
    // A missed frame or two in a continuous animation is a slow frame; a longer gap is a deliberate low-rate
    // redraw (the backdrop drift's 12 Hz timer) after the animation stopped. Intervals within the governor's
    // minimum judged rate always count, so continuous rendering at, say, 36 fps on a 144 Hz display never
    // looks healthy.
    const double gap = std::max(kGapBudgets * budgetMs, 1000.0 / GovernorConfig{}.minFrames);
    std::erase_if(intervalsMs, [gap](double interval) { return interval > gap; });
    return percentile90(std::move(intervalsMs));
}

std::optional<FramePacer::Clock::time_point> FramePacer::wakeUp(Clock::time_point presented, int capFps)
{
    if (capFps <= 0) {
        m_slot.reset();
        return std::nullopt;
    }
    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / capFps));
    // Advance from the slot of the frame just presented, never from an earlier one: the sleep delays the next
    // frame, so measuring from the frame before would let two frames through per period.
    if (!m_slot || presented - (*m_slot + period) > kIdleGap) {
        m_slot = presented + period;  // the first frame, or the first after an idle scene: restart the grid
    } else {
        m_slot = std::max(*m_slot + period, presented);  // after a late frame the next may follow at once
    }
    return *m_slot - kPacingSlack;
}

int FramePacer::capFor(int idleFps, int interactiveFps, Clock::duration sinceInput) noexcept
{
    return sinceInput < kInteractionHold ? interactiveFps : idleFps;
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
    window->installEventFilter(this);
    connect(window, &QQuickWindow::afterRendering, this, &FrameMonitor::onRendered, Qt::DirectConnection);
    connect(window, &QQuickWindow::frameSwapped, this, &FrameMonitor::onSwapped, Qt::DirectConnection);
}

void FrameMonitor::setFrameCap(int fps, int interactiveFps) noexcept
{
    m_frameCapFps.store(std::max(0, fps), std::memory_order_relaxed);
    m_interactiveCapFps.store(std::max(0, interactiveFps), std::memory_order_relaxed);
}

bool FrameMonitor::eventFilter(QObject* watched, QEvent* event)
{
    if (isUserInput(event->type())) {
        {
            const std::scoped_lock lock(m_inputMutex);
            m_lastInput = Clock::now();
        }
        m_inputArrived.notify_all();
    }
    return QObject::eventFilter(watched, event);
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

    if (QThread::currentThread() != QCoreApplication::instance()->thread()) {
        pace(now);
    }
    m_lastSwap = now;
    m_hasLastSwap = true;
}

// Sleeps until shortly before the next allowed frame, then lets the VSync wait absorb the rest. While it sleeps the
// GUI thread cannot sync its next frame, so an input that lifts the cap ends the sleep at once.
void FrameMonitor::pace(Clock::time_point presented)
{
    const int idleFps = m_frameCapFps.load(std::memory_order_relaxed);
    const int interactiveFps = m_interactiveCapFps.load(std::memory_order_relaxed);
    std::unique_lock lock(m_inputMutex);
    const Clock::time_point seen = m_lastInput;
    const auto deadline = m_pacer.wakeUp(presented, FramePacer::capFor(idleFps, interactiveFps, presented - seen));
    if (deadline && *deadline > presented) {
        m_inputArrived.wait_until(lock, *deadline, [&] { return idleFps != interactiveFps && m_lastInput != seen; });
    }
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
    stats.intervalP90Ms = frameIntervalP90(std::move(intervals), stats.budgetMs);
    Q_EMIT measured(stats, elapsed > 0.0 ? frames / elapsed : 0.0);

    if (frames == 0) {
        m_windowOpen.store(false);  // idle: the next frame re-arms the timer
        return;
    }
    QTimer::singleShot(kWindow, this, &FrameMonitor::summarise);
}

}  // namespace noctara::gfx
