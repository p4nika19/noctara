#include "TestUtil.hpp"

#include "core/FileIo.hpp"
#include "gfx/CrashSentinel.hpp"
#include "gfx/GraphicsPolicy.hpp"

#include <QFile>
#include <QTemporaryDir>

#include <map>

using namespace noctara;
using namespace noctara::gfx;

namespace {

ProbeResult gpu(const char* name, DeviceKind kind = DeviceKind::Discrete, std::uint64_t vendor = 0x10de)
{
    ProbeResult result;
    result.ok = true;
    result.deviceName = QString::fromLatin1(name);
    result.kind = kind;
    result.vendorId = vendor;
    return result;
}

ProbeResult failure()
{
    ProbeResult result;
    result.error = QStringLiteral("no device");
    return result;
}

// Mocked probe: fixed results per API, records the order of calls.
struct MockProbe {
    std::map<Api, ProbeResult> results;
    std::vector<Api> calls;

    ProbeFunction function()
    {
        return [this](Api api) {
            calls.push_back(api);
            const auto it = results.find(api);
            return it != results.end() ? it->second : failure();
        };
    }
};

Blocklist realBlocklist()
{
    auto data = readWholeFile(QStringLiteral(NOCTARA_SOURCE_DIR "/resources/data/gpu-blocklist.json"));
    REQUIRE(data.has_value());
    return parseBlocklist(*data);
}

}  // namespace

TEST_SUITE("gfx.policy")
{
    TEST_CASE("safe mode flag and crash recovery never probe")
    {
        MockProbe probe;
        probe.results[Api::OpenGL] = gpu("GeForce");
        auto decision = decide({.safeModeFlag = true}, probe.function(), {});
        CHECK_FALSE(decision.hardware);
        CHECK(decision.safeMode);
        CHECK(decision.reason == Reason::SafeModeFlag);
        decision = decide({.crashSentinelFound = true}, probe.function(), {});
        CHECK(decision.reason == Reason::CrashRecovery);
        CHECK(decision.showNotice);
        CHECK(probe.calls.empty());
    }

    TEST_CASE("disabled acceleration uses software without a notice")
    {
        MockProbe probe;
        const auto decision = decide({.acceleration = Acceleration::Off}, probe.function(), {});
        CHECK_FALSE(decision.hardware);
        CHECK(decision.reason == Reason::UserDisabled);
        CHECK_FALSE(decision.showNotice);
        CHECK(probe.calls.empty());
    }

    TEST_CASE("auto prefers OpenGL and falls back to Vulkan")
    {
        MockProbe probe;
        probe.results[Api::Vulkan] = gpu("GeForce");
        const auto decision = decide({}, probe.function(), {});
        CHECK(decision.hardware);
        CHECK(decision.api == Api::Vulkan);
        CHECK(probe.calls == std::vector<Api>{Api::OpenGL, Api::Vulkan});
        CHECK(decision.reason == Reason::Hardware);
    }

    TEST_CASE("an explicit API that fails falls back with a notice")
    {
        MockProbe probe;
        probe.results[Api::OpenGL] = gpu("Radeon");
        const auto decision = decide({.api = Api::Vulkan}, probe.function(), {});
        CHECK(decision.api == Api::OpenGL);
        CHECK(decision.reason == Reason::ApiFallback);
        CHECK(decision.showNotice);
    }

    TEST_CASE("all probes failing ends in software mode")
    {
        MockProbe probe;
        const auto decision = decide({}, probe.function(), {});
        CHECK_FALSE(decision.hardware);
        CHECK(decision.reason == Reason::ProbeFailed);
        CHECK(decision.showNotice);
    }

    TEST_CASE("software rasterizers are rejected in Auto but allowed when forced On")
    {
        MockProbe probe;
        probe.results[Api::OpenGL] = gpu("llvmpipe (LLVM 19.1.0, 256 bits)", DeviceKind::Unknown);
        auto decision = decide({}, probe.function(), {});
        CHECK_FALSE(decision.hardware);
        CHECK(decision.reason == Reason::SoftwareRasterizer);

        decision = decide({.acceleration = Acceleration::On}, probe.function(), {});
        CHECK(decision.hardware);
        CHECK_FALSE(decision.liquidGlassAllowed);
    }

    TEST_CASE("blocklist data file: parsing and actions")
    {
        const Blocklist list = realBlocklist();
        REQUIRE(list.size() >= 2);

        MockProbe vm;
        vm.results[Api::OpenGL] = gpu("SVGA3D; build: RELEASE", DeviceKind::Virtual, 0x15ad);
        const auto vmDecision = decide({}, vm.function(), list);
        CHECK(vmDecision.hardware);
        CHECK_FALSE(vmDecision.liquidGlassAllowed);

        MockProbe swiftshader;
        swiftshader.results[Api::OpenGL] = gpu("SwiftShader Device", DeviceKind::Discrete);
        CHECK(decide({}, swiftshader.function(), list).reason == Reason::Blocklisted);

        Blocklist avoid = parseBlocklist(R"({"entries":[{"id":"x","deviceName":"Buggy","api":"opengl",
                                                        "action":"avoid-api","reason":"test"}]})");
        MockProbe buggy;
        buggy.results[Api::OpenGL] = gpu("Buggy GPU");
        buggy.results[Api::Vulkan] = gpu("Buggy GPU");
        const auto avoided = decide({}, buggy.function(), avoid);
        CHECK(avoided.hardware);
        CHECK(avoided.api == Api::Vulkan);
    }

    TEST_CASE("invalid blocklist entries are skipped")
    {
        const Blocklist list = parseBlocklist(R"({"entries":[{"id":"a","action":"explode"},{"action":"software"},
                                                             {"id":"ok","deviceName":"x","action":"software"}]})");
        REQUIRE(list.size() == 1);
        CHECK(list.front().id == QStringLiteral("ok"));
        CHECK(parseBlocklist("not json").empty());
    }

    TEST_CASE("probe results round-trip through the cache format")
    {
        ProbeResult original = gpu("GTX 1070");
        original.api = Api::Vulkan;
        original.driver = QStringLiteral("580.1");
        original.adapterCount = 2;
        const ProbeResult copy = ProbeResult::fromJson(original.toJson());
        CHECK(copy.ok);
        CHECK(copy.api == Api::Vulkan);
        CHECK(copy.deviceName == original.deviceName);
        CHECK(copy.kind == DeviceKind::Discrete);
        CHECK(copy.adapterCount == 2);
    }
}

TEST_SUITE("gfx.sentinel")
{
    TEST_CASE("an armed sentinel left behind marks the next launch as crashed")
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("state/graphics-init.sentinel"));
        {
            CrashSentinel first(path);
            CHECK_FALSE(first.previousLaunchCrashed());
            first.arm();
            CHECK(QFile::exists(path));
            // Simulate a crash: copy the file aside so the destructor's cleanup does not hide it.
            REQUIRE(QFile::copy(path, path + QStringLiteral(".crash")));
        }
        CHECK_FALSE(QFile::exists(path));
        REQUIRE(QFile::rename(path + QStringLiteral(".crash"), path));

        CrashSentinel second(path);
        CHECK(second.previousLaunchCrashed());
        second.arm();
        second.disarm();
        CHECK_FALSE(QFile::exists(path));
        CrashSentinel third(path);
        CHECK_FALSE(third.previousLaunchCrashed());
    }
}
