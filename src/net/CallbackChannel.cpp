#include "net/CallbackChannel.hpp"

#include <QThread>

#ifdef NOCTARA_TSAN
#include <QSet>

#include <mutex>
#endif

namespace noctara::net {

namespace {

#ifdef NOCTARA_TSAN
// A closure still queued when its context is destroyed never runs: ~QObject drops the posted event, and with it
// the copy of the closure the posting thread made, so the receiving side's barrier is never taken. ~QObject emits
// destroyed() before it removes posted events, so taking the barrier there makes that drop visible too. One
// connection per context, however many requests it makes.
void watchContext(QObject* context)
{
    static std::mutex mutex;
    static QSet<QObject*> watched;
    {
        const std::scoped_lock lock(mutex);
        if (watched.contains(context)) {
            return;
        }
        watched.insert(context);
    }
    QObject::connect(
        context, &QObject::destroyed, context,
        [context] {
            handoff::barrier();
            const std::scoped_lock lock(mutex);
            watched.remove(context);
        },
        Qt::DirectConnection);
}
#else
void watchContext(QObject* /*context*/) {}
#endif

}  // namespace

CallbackChannel::CallbackChannel(QObject* context)
{
    static const int registered = qRegisterMetaType<std::function<void()>>();
    (void)registered;
    watchContext(context);
    connect(
        this, &CallbackChannel::posted, context,
        [](const std::function<void()>& closure) {
            handoff::barrier();
            closure();
        },
        Qt::QueuedConnection);
}

void ChannelDeleter::operator()(CallbackChannel* channel) const noexcept
{
    handoff::send([channel] { delete channel; });  // NOLINT(cppcoreguidelines-owning-memory)
}

ChannelPtr makeChannel(QObject* context, const std::shared_ptr<std::atomic<bool>>& cancelled, QThread* owner)
{
    ChannelPtr channel(new CallbackChannel(context));  // NOLINT(cppcoreguidelines-owning-memory)
    // The receiver going away cancels the request; only an atomic store, so a direct connection is safe. A weak
    // reference: the connection must not keep the request state alive.
    QObject::connect(
        context, &QObject::destroyed, channel.get(),
        [weak = std::weak_ptr(cancelled)] {
            // The channel (and with it this connection) is deleted on the owner's thread under the same lock.
            handoff::send([&] {
                if (const auto locked = weak.lock()) {
                    locked->store(true, std::memory_order_relaxed);
                }
            });
        },
        Qt::DirectConnection);
    channel->moveToThread(owner);
    return channel;
}

}  // namespace noctara::net
