#pragma once

#include "gfx/QualityGovernor.hpp"

#include <QObject>
#include <QPointer>
#include <QQuickWindow>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <vector>

namespace noctara::gfx {

// Paces presented frames to a frame-rate cap on a grid of one period per frame: after a frame was presented,
// the render thread may start the next one shortly before the next grid slot, so the following VSync wait
// absorbs the difference. After a late frame the next one may follow at once; after an idle scene the grid
// restarts from the presented frame, so the first frames are not let through in a burst. Pure; render thread
// only.
class FramePacer {
public:
    using Clock = std::chrono::steady_clock;

    // When to start the next frame after one was presented at `presented`; nothing for no cap (0).
    [[nodiscard]] std::optional<Clock::time_point> wakeUp(Clock::time_point presented, int capFps);

private:
    std::optional<Clock::time_point> m_slot;  // when the next frame may be presented
};

// Measures rendering on a window and paces it to the frame-rate cap.
//
// On the render thread it records the GPU time of each frame (QRhi timestamps, when the backend supports them)
// and the interval between presented frames; on the GUI thread it summarises them once per second of
// rendering. Nothing runs while the window is idle: the summary timer is armed by the first frame after a
// pause, so a static scene causes no wake-ups.
class FrameMonitor : public QObject {
    Q_OBJECT

public:
    explicit FrameMonitor(QObject* parent = nullptr);

    // Enables GPU timestamps, so it must be called before the window is first shown.
    void attach(QQuickWindow* window);
    // 0 = no cap beyond VSync. Applied by sleeping on the render thread after a frame is presented; never on
    // the GUI thread (the software renderer draws there), where it would delay input handling.
    void setFrameCap(int fps) noexcept;

Q_SIGNALS:
    void measured(const noctara::gfx::FrameWindowStats& stats, double fps);

private:
    using Clock = std::chrono::steady_clock;

    void onRendered();  // render thread
    void onSwapped();   // render thread
    void summarise();   // GUI thread

    QPointer<QQuickWindow> m_window;
    std::atomic<int> m_frameCapFps{0};
    std::atomic<bool> m_windowOpen{false};

    // Render thread only.
    FramePacer m_pacer;
    Clock::time_point m_lastSwap;
    bool m_hasLastSwap = false;

    std::mutex m_mutex;
    std::vector<double> m_gpuMs;        // guarded by m_mutex
    std::vector<double> m_intervalsMs;  // guarded by m_mutex
    int m_frames = 0;                   // guarded by m_mutex
    Clock::time_point m_windowStart;    // guarded by m_mutex
};

// 90th percentile of the samples (by partial sort); 0 for an empty input.
[[nodiscard]] double percentile90(std::vector<double> samples);

}  // namespace noctara::gfx
