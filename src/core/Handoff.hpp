#pragma once

#include <QMetaObject>
#include <QObject>

#include <future>
#include <mutex>
#include <utility>

// Qt's event queue orders a posted closure after the code that posted it, but the distro's Qt is not built with
// ThreadSanitizer, so TSan cannot see that ordering and reports every hand-over of captured data as a race. In
// TSan builds, posting happens under one mutex that the receiving side takes again before running the closure,
// which makes the same ordering visible. Other builds compile to a plain queued call.

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define NOCTARA_TSAN 1
#endif
#endif

namespace noctara::handoff {

#ifdef NOCTARA_TSAN
inline std::mutex& mutex()
{
    static std::mutex instance;
    return instance;
}

// Called first by the receiving side: everything the poster did before send() is now visible.
inline void barrier()
{
    const std::scoped_lock lock(mutex());
}

template <class Function> auto receive(Function function)
{
    return [function = std::move(function)]() mutable {
        barrier();
        function();
    };
}

template <class Post> void send(Post post)
{
    const std::scoped_lock lock(mutex());
    post();
}
#else
inline void barrier() {}

template <class Function> Function receive(Function function)
{
    return function;
}

template <class Post> void send(Post post)
{
    post();
}
#endif

// Runs `function` later on `target`'s thread.
template <class Function> void post(QObject* target, Function function)
{
    send([&] { QMetaObject::invokeMethod(target, receive(std::move(function)), Qt::QueuedConnection); });
}

// Runs `function` on `target`'s thread and waits for it. A promise rather than Qt::BlockingQueuedConnection makes
// the hand-back visible to ThreadSanitizer (Qt's own semaphore is inside the uninstrumented Qt library).
template <class Function> void runAndWait(QObject* target, Function function)
{
    std::promise<void> done;
    post(target, [&] {
        function();
        done.set_value();
    });
    done.get_future().wait();
}

}  // namespace noctara::handoff
