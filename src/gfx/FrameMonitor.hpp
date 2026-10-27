#pragma once

#include "gfx/QualityGovernor.hpp"

#include <QObject>
#include <QPointer>
#include <QQuickWindow>

#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

namespace noctara::gfx {

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
