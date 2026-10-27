#pragma once

#include "Doctest.hpp"

#include "core/Error.hpp"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QString>

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

// The error code of a result that is expected to fail; nullopt (never equal to a code) if it succeeded.
template <class T> std::optional<ErrorCode> errorOf(const Result<T>& result)
{
    return result ? std::nullopt : std::optional(result.error().code);
}

// Spins the event loop until `predicate` holds or the timeout elapses.
template <class Predicate> bool waitUntil(Predicate predicate, int timeoutMs = 3000)
{
    QDeadlineTimer deadline(timeoutMs);
    while (!predicate()) {
        if (deadline.hasExpired()) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

}  // namespace noctara::test
