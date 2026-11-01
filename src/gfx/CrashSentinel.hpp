#pragma once

#include <QString>

#include <functional>

class QObject;
class QQuickWindow;

namespace noctara::gfx {

// A file that exists only between "about to initialise graphics" and "first frame presented".
// If it is still there at the next start, that launch crashed during graphics init → Safe Mode.
// The destructor removes it on orderly shutdown; a crash leaves it behind by design.
class CrashSentinel {
public:
    explicit CrashSentinel(QString path);
    ~CrashSentinel();

    CrashSentinel(const CrashSentinel&) = delete;
    CrashSentinel& operator=(const CrashSentinel&) = delete;

    [[nodiscard]] bool previousLaunchCrashed() const noexcept { return m_foundAtStartup; }
    void arm();
    void disarm();

private:
    QString m_path;
    bool m_foundAtStartup = false;
    bool m_armed = false;
};

// Keeps `sentinel` armed only while `window`'s graphics are being set up, and disarms it on the first frame
// (then runs `firstFrame` on `context`'s thread). A window that starts hidden (in the tray) initialises nothing,
// so the sentinel is disarmed at once and armed again when the window is first shown: otherwise any kill during
// a long session in the tray would start the next launch in Safe Mode.
void coverFirstFrame(CrashSentinel& sentinel, QQuickWindow& window, const QObject& context,
                     std::function<void()> firstFrame);

}  // namespace noctara::gfx
