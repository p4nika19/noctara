#pragma once

#include "Doctest.hpp"

#include "core/Error.hpp"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QString>

#include <ostream>

inline std::ostream& operator<<(std::ostream& stream, const QString& text)
{
    return stream << text.toStdString();
}

namespace noctara {
inline std::ostream& operator<<(std::ostream& stream, ErrorCode code)
{
    return stream << errorCodeName(code);
}
}  // namespace noctara

namespace noctara::test {

// Spins the event loop until `predicate` holds or the timeout elapses.
template <class Predicate>
bool waitUntil(Predicate predicate, int timeoutMs = 3000)
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
