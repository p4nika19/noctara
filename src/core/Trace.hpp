#pragma once

#include <cstdint>

// Compile-time-switchable tracing. With `xmake f --trace=y` (NOCTARA_TRACE=1) NOCTARA_TRACE_SCOPE("name") records the
// time a scope takes and NOCTARA_TRACE_INSTANT("name") a moment, on whichever thread runs them; at exit the events
// are written as Chrome trace JSON (open in Perfetto or chrome://tracing) to $NOCTARA_TRACE_FILE, by default
// noctara-trace.json in the working directory. Without the option both macros compile to nothing. Names must be
// string literals.

#if defined(NOCTARA_TRACE) && NOCTARA_TRACE

namespace noctara::trace {

[[nodiscard]] std::int64_t now() noexcept;  // ns, monotonic
// endNs < 0 records an instant.
void record(const char* name, std::int64_t beginNs, std::int64_t endNs);

class Scope {
public:
    explicit Scope(const char* name) noexcept
        : m_name(name)
        , m_begin(now())
    {
    }
    ~Scope() { record(m_name, m_begin, now()); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;

private:
    const char* m_name;
    std::int64_t m_begin;
};

}  // namespace noctara::trace

#define NOCTARA_TRACE_JOIN2(a, b) a##b
#define NOCTARA_TRACE_JOIN(a, b) NOCTARA_TRACE_JOIN2(a, b)
#define NOCTARA_TRACE_SCOPE(name) const ::noctara::trace::Scope NOCTARA_TRACE_JOIN(noctaraTrace, __LINE__)(name)
#define NOCTARA_TRACE_INSTANT(name) ::noctara::trace::record(name, ::noctara::trace::now(), -1)

#else

#define NOCTARA_TRACE_SCOPE(name) static_cast<void>(0)
#define NOCTARA_TRACE_INSTANT(name) static_cast<void>(0)

#endif
