// The test helpers themselves.
#include "TestUtil.hpp"

#include <QElapsedTimer>

#include <chrono>
#include <sys/resource.h>

using namespace noctara;

namespace {

// CPU time (user + system) the calling thread has used so far.
std::chrono::microseconds threadCpuTime()
{
    rusage usage{};
    ::getrusage(RUSAGE_THREAD, &usage);
    auto toMicros = [](const timeval& value) {
        return std::chrono::seconds(value.tv_sec) + std::chrono::microseconds(value.tv_usec);
    };
    return toMicros(usage.ru_utime) + toMicros(usage.ru_stime);
}

}  // namespace

TEST_SUITE("test.util")
{
    TEST_CASE("waitUntil sleeps while nothing happens instead of spinning")
    {
        QElapsedTimer wall;
        wall.start();
        const std::chrono::microseconds before = threadCpuTime();
        CHECK_FALSE(test::waitUntil([] { return false; }, 300));
        const auto cpu = std::chrono::duration_cast<std::chrono::milliseconds>(threadCpuTime() - before);
        const qint64 elapsed = wall.elapsed();
        INFO("cpu ", cpu.count(), " ms over ", elapsed, " ms");
        CHECK(elapsed >= 300);
        CHECK(cpu.count() * 4 < elapsed);
    }

    TEST_CASE("waitUntil still notices a condition that changes without an event")
    {
        QElapsedTimer wall;
        wall.start();
        CHECK(test::waitUntil([&] { return wall.elapsed() >= 50; }, 1000));
        CHECK(wall.elapsed() < 500);
    }
}
