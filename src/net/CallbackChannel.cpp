#include "net/CallbackChannel.hpp"

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

}  // namespace noctara::net
