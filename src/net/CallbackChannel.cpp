#include "net/CallbackChannel.hpp"

#include <QThread>

namespace noctara::net {

CallbackChannel::CallbackChannel(QObject* context)
{
    static const int registered = qRegisterMetaType<std::function<void()>>();
    (void)registered;
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
