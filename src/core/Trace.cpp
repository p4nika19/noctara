#include "core/Trace.hpp"

#if defined(NOCTARA_TRACE) && NOCTARA_TRACE

#include <QByteArray>
#include <QtGlobal>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <mutex>
#include <unistd.h>
#include <vector>

namespace noctara::trace {

namespace {

struct Event {
    const char* name;
    std::int64_t begin;
    std::int64_t end;
    pid_t thread;
};

struct Recorder {
    std::mutex mutex;
    std::vector<Event> events;
};

Recorder& recorder()
{
    static Recorder* instance = [] {
        auto* created = new Recorder;  // NOLINT(cppcoreguidelines-owning-memory): lives until exit, written there
        created->events.reserve(16384);
        (void)std::atexit([] {
            Recorder& r = recorder();
            const std::scoped_lock lock(r.mutex);
            const QByteArray configured = qgetenv("NOCTARA_TRACE_FILE");
            std::ofstream out(configured.isEmpty() ? std::string("noctara-trace.json") : configured.toStdString());
            if (!out) {
                return;
            }
            std::int64_t origin = r.events.empty() ? 0 : r.events.front().begin;
            for (const Event& e : r.events) {
                origin = std::min(origin, e.begin);
            }
            const pid_t pid = ::getpid();
            // std::format is locale-independent: timestamps keep their decimal point under any locale.
            const auto micros = [](std::int64_t ns) {
                return static_cast<double>(ns) / 1000.0;
            };
            out << R"({"traceEvents":[)";
            for (std::size_t i = 0; i < r.events.size(); ++i) {
                const Event& e = r.events[i];
                out << (i == 0 ? "" : ",");
                if (e.end < 0) {
                    out << std::format(R"({{"name":"{}","ph":"i","s":"p","ts":{:.3f},"pid":{},"tid":{}}})", e.name,
                                       micros(e.begin - origin), pid, e.thread);
                } else {
                    out << std::format(R"({{"name":"{}","ph":"X","ts":{:.3f},"dur":{:.3f},"pid":{},"tid":{}}})", e.name,
                                       micros(e.begin - origin), micros(e.end - e.begin), pid, e.thread);
                }
            }
            out << "]}\n";
        });
        return created;
    }();
    return *instance;
}

}  // namespace

std::int64_t now() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void record(const char* name, std::int64_t beginNs, std::int64_t endNs)
{
    Recorder& r = recorder();
    const std::scoped_lock lock(r.mutex);
    r.events.push_back({name, beginNs, endNs, ::gettid()});
}

}  // namespace noctara::trace

#endif
