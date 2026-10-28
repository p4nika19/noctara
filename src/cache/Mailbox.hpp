#pragma once

#include "core/Handoff.hpp"

#include <QObject>

#include <memory>
#include <mutex>

namespace noctara::cache {

// Posts closures to an object's thread from any thread, safely even if the object is being destroyed: the
// owner closes the mailbox in its destructor, after which posts are dropped. Closures queued before that are
// discarded by Qt together with the object. Shared by handles that may outlive their owner.
class Mailbox {
public:
    explicit Mailbox(QObject* target)
        : m_target(target)
    {
    }

    template <class Function> void post(Function function)
    {
        const std::scoped_lock lock(m_mutex);
        if (m_target != nullptr) {
            handoff::post(m_target, std::move(function));
        }
    }

    void close()
    {
        const std::scoped_lock lock(m_mutex);
        m_target = nullptr;
    }

private:
    std::mutex m_mutex;
    QObject* m_target;
};

using MailboxPtr = std::shared_ptr<Mailbox>;

}  // namespace noctara::cache
