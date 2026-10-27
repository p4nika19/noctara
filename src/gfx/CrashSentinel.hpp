#pragma once

#include <QString>

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

}  // namespace noctara::gfx
