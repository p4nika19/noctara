#pragma once

#include "Doctest.hpp"

#include "core/Error.hpp"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QString>
#include <QTimer>

#include <optional>
#include <ostream>
#include <string>

inline std::ostream& operator<<(std::ostream& stream, const QString& text)
{
    return stream << text.toStdString();
}

namespace noctara {
inline std::ostream& operator<<(std::ostream& stream, ErrorCode code)
{
    return stream << errorCodeName(code);
}
inline std::ostream& operator<<(std::ostream& stream, const std::optional<ErrorCode>& code)
{
    return code ? stream << *code : stream << "success";
}
}  // namespace noctara

namespace noctara::test {

// For assertion messages: doctest may evaluate them even when the check passes (e.g. with -s), so never call
// .error() on a result that holds a value.
template <class T> std::string describe(const Result<T>& result)
{
    return result ? std::string("ok") : result.error().describe();
}
inline std::string describe(const std::optional<Error>& error)
{
    return error ? error->describe() : std::string("ok");
}

// The error code of a result that is expected to fail; nullopt (never equal to a code) if it succeeded.
template <class T> std::optional<ErrorCode> errorOf(const Result<T>& result)
{
    return result ? std::nullopt : std::optional(result.error().code);
}

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define NOCTARA_TEST_TSAN 1
#endif
#endif

// ThreadSanitizer slows everything down several times and now and then stalls a thread for seconds: waits for
// asynchronous results get this much more time there. Other builds keep the stated timeouts.
#ifdef NOCTARA_TEST_TSAN
inline constexpr int kTimeoutScale = 10;
#else
inline constexpr int kTimeoutScale = 1;
#endif

// Runs the event loop until `predicate` holds or the timeout (scaled by kTimeoutScale) elapses. While no events
// arrive the thread sleeps; a short timer still wakes it to re-check conditions that change without an event (for
// example state set by another thread).
template <class Predicate> bool waitUntil(Predicate predicate, int timeoutMs = 3000)
{
    QDeadlineTimer deadline(static_cast<qint64>(timeoutMs) * kTimeoutScale);
    QTimer wake;
    wake.setTimerType(Qt::PreciseTimer);
    wake.start(2);
    while (!predicate()) {
        if (deadline.hasExpired()) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents);
    }
    return true;
}

}  // namespace noctara::test
