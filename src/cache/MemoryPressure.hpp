#pragma once

#include "core/UniqueFd.hpp"

#include <QElapsedTimer>
#include <QObject>
#include <QSocketNotifier>

#include <memory>

namespace noctara::cache {

// Event-driven memory-pressure signal from the kernel's pressure stall information (/proc/pressure/memory):
// a PSI trigger wakes the event loop only when tasks stall on memory, so it costs nothing while idle. Without
// PSI (old kernels, containers) pressure() simply never fires.
class MemoryPressureMonitor : public QObject {
    Q_OBJECT

public:
    explicit MemoryPressureMonitor(QObject* parent = nullptr);

    [[nodiscard]] bool available() const noexcept { return m_fd.valid(); }

Q_SIGNALS:
    void pressure();  // at most every few seconds

private:
    UniqueFd m_fd;
    std::unique_ptr<QSocketNotifier> m_notifier;
    QElapsedTimer m_last;
};

}  // namespace noctara::cache
