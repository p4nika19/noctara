#include "cache/MemoryPressure.hpp"

#include "core/Log.hpp"

#include <fcntl.h>
#include <unistd.h>

namespace noctara::cache {

namespace {

// 150 ms of partial stall within a 2 s window (unprivileged triggers need a window that is a multiple of 2 s).
constexpr char kTrigger[] = "some 150000 2000000";
constexpr qint64 kMinIntervalMs = 5000;

}  // namespace

MemoryPressureMonitor::MemoryPressureMonitor(QObject* parent)
    : QObject(parent)
    , m_fd(::open("/proc/pressure/memory", O_RDWR | O_NONBLOCK | O_CLOEXEC))
{
    if (!m_fd) {
        return;
    }
    if (::write(m_fd.get(), kTrigger, sizeof(kTrigger)) < 0) {
        qCDebug(lcCache) << "memory pressure trigger unavailable";
        m_fd.reset();
        return;
    }
    m_notifier = std::make_unique<QSocketNotifier>(m_fd.get(), QSocketNotifier::Exception);
    connect(m_notifier.get(), &QSocketNotifier::activated, this, [this] {
        if (!m_last.isValid() || m_last.elapsed() >= kMinIntervalMs) {
            m_last.start();
            qCInfo(lcCache) << "memory pressure: trimming the RAM caches";
            Q_EMIT pressure();
        }
    });
}

}  // namespace noctara::cache
