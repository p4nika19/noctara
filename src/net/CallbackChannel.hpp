#pragma once

#include "core/Handoff.hpp"

#include <QMetaType>
#include <QObject>

#include <atomic>
#include <functional>
#include <memory>

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

// Channels are deleted on their owner's thread, directly (never from inside their own signal) and under the
// handoff lock, so the caller thread's cancel-on-destroy connection cannot overlap with the deletion.
struct ChannelDeleter {
    void operator()(CallbackChannel* channel) const noexcept;
};
using ChannelPtr = std::unique_ptr<CallbackChannel, ChannelDeleter>;

// A channel to `context` for a request of a transport living on `owner`: destroying `context` sets `cancelled`
// (held weakly), and the channel is moved to `owner`, where it must be destroyed.
[[nodiscard]] ChannelPtr makeChannel(QObject* context, const std::shared_ptr<std::atomic<bool>>& cancelled,
                                     QThread* owner);

}  // namespace noctara::net
