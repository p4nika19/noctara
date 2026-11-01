#include "TestUtil.hpp"

#include "core/FileIo.hpp"
#include "gfx/CrashSentinel.hpp"
#include "gfx/FrameMonitor.hpp"
#include "gfx/GpuProbe.hpp"
#include "gfx/GraphicsPolicy.hpp"
#include "gfx/QualityGovernor.hpp"

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

    TEST_CASE("auto prefers Vulkan and falls back to OpenGL")
    {
        MockProbe probe;
        probe.results[Api::Vulkan] = gpu("GeForce");
        probe.results[Api::OpenGL] = gpu("GeForce");
        auto decision = decide({}, probe.function(), {});
        CHECK(decision.hardware);
        CHECK(decision.api == Api::Vulkan);
        CHECK(probe.calls == std::vector<Api>{Api::Vulkan});
        CHECK(decision.reason == Reason::Hardware);

        MockProbe noVulkan;
        noVulkan.results[Api::OpenGL] = gpu("Intel HD Graphics 4000", DeviceKind::Integrated, 0x8086);
        decision = decide({}, noVulkan.function(), {});
        CHECK(decision.api == Api::OpenGL);
        CHECK(noVulkan.calls == std::vector<Api>{Api::Vulkan, Api::OpenGL});
        CHECK_FALSE(decision.showNotice);  // Auto: no API was requested, so this is not a fallback
    }

    TEST_CASE("a CPU-only Vulkan (lavapipe) does not hide a real GPU behind OpenGL")
    {
        MockProbe probe;
        probe.results[Api::Vulkan] = gpu("llvmpipe (LLVM 19.1.0, 256 bits)", DeviceKind::Cpu);
        probe.results[Api::OpenGL] = gpu("AMD Radeon Graphics", DeviceKind::Integrated, 0x1002);
        const auto decision = decide({}, probe.function(), {});
        CHECK(decision.hardware);
        CHECK(decision.api == Api::OpenGL);
    }

    TEST_CASE("the shipped blocklist's CPU rasterizer entry does not end the search either")
    {
        MockProbe probe;
        probe.results[Api::Vulkan] = gpu("llvmpipe (LLVM 19.1.0, 256 bits)", DeviceKind::Cpu);
        probe.results[Api::OpenGL] = gpu("AMD Radeon Graphics", DeviceKind::Integrated, 0x1002);
        const auto decision = decide({}, probe.function(), realBlocklist());
        CHECK(decision.hardware);
        CHECK(decision.api == Api::OpenGL);
        CHECK(probe.calls == std::vector<Api>{Api::Vulkan, Api::OpenGL});

        // A real GPU blocklisted to software still ends the search: its other API is the same device.
        Blocklist list = parseBlocklist(R"({"entries":[{"id":"x","deviceName":"Broken","action":"software",
                                                       "reason":"test"}]})");
        MockProbe broken;
        broken.results[Api::Vulkan] = gpu("Broken GPU");
        broken.results[Api::OpenGL] = gpu("Broken GPU");
        const auto blocked = decide({}, broken.function(), list);
        CHECK_FALSE(blocked.hardware);
        CHECK(blocked.reason == Reason::Blocklisted);
        CHECK(broken.calls == std::vector<Api>{Api::Vulkan});
    }

    TEST_CASE("an explicit API that fails falls back with a notice")
    {
        MockProbe probe;
        probe.results[Api::OpenGL] = gpu("Radeon");
        const auto decision = decide({.api = Api::Vulkan}, probe.function(), {});
        CHECK(decision.api == Api::OpenGL);
        CHECK(probe.calls == std::vector<Api>{Api::Vulkan, Api::OpenGL});
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

namespace {

FrameWindowStats gpuWindow(double gpuMs, int frames = 60)
{
    FrameWindowStats stats;
    stats.frames = frames;
    stats.budgetMs = 1000.0 / 60.0;
    stats.gpuP90Ms = gpuMs;
    stats.intervalP90Ms = stats.budgetMs;
    return stats;
}

}  // namespace

TEST_SUITE("gfx.governor")
{
    TEST_CASE("steps down one tier per sustained overload, in the documented order")
    {
        QualityGovernor governor;
        double now = 0.0;
        const auto overloaded = gpuWindow(15.0);                       // 90% of a 16.7 ms budget
        CHECK_FALSE(governor.feed(overloaded, now += 1).has_value());  // one bad second is not enough
        CHECK(governor.feed(overloaded, now += 1) == Tier::Medium);
        CHECK_FALSE(governor.feed(overloaded, now += 1).has_value());
        CHECK(governor.feed(overloaded, now += 1) == Tier::Low);
        governor.feed(overloaded, now += 1);
        CHECK(governor.feed(overloaded, now += 1) == Tier::Kawase);
        governor.feed(overloaded, now += 1);
        CHECK(governor.feed(overloaded, now += 1) == Tier::Solid);
        for (int i = 0; i < 6; ++i) {
            CHECK_FALSE(governor.feed(overloaded, now += 1).has_value());  // floor
        }
        CHECK(governor.tier() == Tier::Solid);
    }

    TEST_CASE("idle windows are not judged; a single fluctuation resets the streak")
    {
        QualityGovernor governor;
        double now = 0.0;
        CHECK_FALSE(governor.feed(gpuWindow(30.0, 5), now += 1).has_value());  // too few frames
        CHECK_FALSE(governor.feed(gpuWindow(15.0), now += 1).has_value());
        CHECK_FALSE(governor.feed(gpuWindow(10.0), now += 1).has_value());  // neutral: streak broken
        CHECK_FALSE(governor.feed(gpuWindow(15.0), now += 1).has_value());
        CHECK(governor.tier() == Tier::High);
    }

    TEST_CASE("steps up conservatively, and backs off when it oscillates")
    {
        GovernorConfig config;
        QualityGovernor governor(config);
        double now = 0.0;
        governor.feed(gpuWindow(15.0), now += 1);
        REQUIRE(governor.feed(gpuWindow(15.0), now += 1) == Tier::Medium);

        // Healthy windows only count after the cooldown since the last change.
        std::optional<Tier> raised;
        const double changedAt = now;
        while (!raised && now < 100) {
            raised = governor.feed(gpuWindow(2.0), now += 1);
        }
        REQUIRE(raised == Tier::High);
        CHECK(now - changedAt >= config.upCooldownS);

        // Trouble right after stepping up doubles the cooldown.
        governor.feed(gpuWindow(15.0), now += 1);
        CHECK(governor.feed(gpuWindow(15.0), now += 1) == Tier::Medium);
        CHECK(governor.cooldownSeconds() == doctest::Approx(config.upCooldownS * 2));

        governor.reset();
        CHECK(governor.tier() == Tier::High);
        CHECK(governor.cooldownSeconds() == doctest::Approx(config.upCooldownS));
    }

    TEST_CASE("without GPU timestamps the frame intervals decide")
    {
        QualityGovernor governor;
        FrameWindowStats stats;
        stats.frames = 40;
        stats.budgetMs = 1000.0 / 144.0;
        stats.intervalP90Ms = stats.budgetMs * 2.0;  // every other frame missed
        governor.feed(stats, 1);
        CHECK(governor.feed(stats, 2) == Tier::Medium);
    }

    TEST_CASE("percentile90 of frame samples")
    {
        CHECK(percentile90({}) == 0.0);
        std::vector<double> samples;
        for (int i = 1; i <= 100; ++i) {
            samples.push_back(i);
        }
        CHECK(percentile90(samples) == doctest::Approx(90.0));
    }
}

TEST_SUITE("gfx.effects")
{
    TEST_CASE("auto quality follows the governor tier")
    {
        EffectsInputs inputs;
        CHECK(resolveEffects(inputs).quality == 2);
        CHECK_FALSE(resolveEffects(inputs).governed);
        inputs.governorTier = Tier::Medium;
        CHECK(resolveEffects(inputs).quality == 1);
        CHECK(resolveEffects(inputs).governed);
        inputs.governorTier = Tier::Kawase;
        CHECK(resolveEffects(inputs).styleCap == QStringLiteral("kawase"));
        inputs.governorTier = Tier::Solid;
        CHECK(resolveEffects(inputs).styleCap == QStringLiteral("solid"));
    }

    TEST_CASE("a manual quality ignores the governor")
    {
        EffectsInputs inputs;
        inputs.quality = QualitySetting::Medium;
        inputs.governorTier = Tier::Solid;
        const EffectsState state = resolveEffects(inputs);
        CHECK(state.quality == 1);
        CHECK(state.styleCap.isEmpty());
        CHECK_FALSE(state.governed);
    }

    TEST_CASE("energy saving: still backdrop, Dual Kawase at most, 30 fps")
    {
        EffectsInputs inputs;
        inputs.energySaving = true;
        EffectsState state = resolveEffects(inputs);
        CHECK(state.energySaving);
        CHECK_FALSE(state.backdropAnimation);
        CHECK(state.styleCap == QStringLiteral("kawase"));
        CHECK(state.quality == 1);
        CHECK(state.frameCapFps == 30);

        inputs.frameRateCap = QStringLiteral("60");
        CHECK(resolveEffects(inputs).frameCapFps == 30);
        inputs.governorTier = Tier::Solid;
        CHECK(resolveEffects(inputs).styleCap == QStringLiteral("solid"));  // the stricter cap wins
        inputs.energySaving = false;
        inputs.governorTier = Tier::High;
        CHECK(resolveEffects(inputs).frameCapFps == 60);
        inputs.frameRateCap = QStringLiteral("unlimited");
        CHECK(resolveEffects(inputs).frameCapFps == 0);
    }

    TEST_CASE("the governor does not act in the software renderer")
    {
        EffectsInputs inputs;
        inputs.hardware = false;
        inputs.governorTier = Tier::Low;
        const EffectsState state = resolveEffects(inputs);
        CHECK_FALSE(state.governed);
        CHECK(state.quality == 2);
    }
}

TEST_SUITE("gfx.device")
{
    TEST_CASE("the GPU preference picks the first physical device of the wanted type")
    {
        const std::vector<DeviceKind> hybrid{DeviceKind::Cpu, DeviceKind::Integrated, DeviceKind::Discrete};
        CHECK(preferredDeviceIndex(hybrid, DevicePreference::Auto) == -1);
        CHECK(preferredDeviceIndex(hybrid, DevicePreference::Integrated) == 1);
        CHECK(preferredDeviceIndex(hybrid, DevicePreference::Discrete) == 2);
        CHECK(preferredDeviceIndex({DeviceKind::Discrete}, DevicePreference::Integrated) == -1);
        CHECK(parseDevicePreference(QStringLiteral("discrete")) == DevicePreference::Discrete);
        CHECK(parseDevicePreference(QStringLiteral("whatever")) == DevicePreference::Auto);
    }

    TEST_CASE("the chosen device index survives the probe cache")
    {
        ProbeResult result = gpu("GeForce");
        result.api = Api::Vulkan;
        result.deviceIndex = 2;
        CHECK(ProbeResult::fromJson(result.toJson()).deviceIndex == 2);
        CHECK(ProbeResult::fromJson(QJsonObject{}).deviceIndex == -1);
    }
}
