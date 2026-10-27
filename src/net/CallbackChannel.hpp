#pragma once

#include "core/Handoff.hpp"

#include <QMetaType>
#include <QObject>

#include <functional>

Q_DECLARE_METATYPE(std::function<void()>)

namespace noctara::net {

// Runs closures on a context object's thread, in posting order, from any thread. Closures still queued when
// the context is destroyed are dropped by Qt (queued connections to a deleted receiver are discarded), so
// callers never need to guard against their receiver going away.
class CallbackChannel : public QObject {
    Q_OBJECT

public:
    explicit CallbackChannel(QObject* context);

    void post(std::function<void()> closure)
    {
        handoff::send([&] { Q_EMIT posted(std::move(closure)); });
    }

Q_SIGNALS:
    void posted(std::function<void()> closure);
};

}  // namespace noctara::net
