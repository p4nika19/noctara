#include "TestUtil.hpp"

#include "core/FileIo.hpp"
#include "gfx/CrashSentinel.hpp"
#include "gfx/FrameMonitor.hpp"
#include "gfx/GpuProbe.hpp"
#include "gfx/GraphicsPolicy.hpp"
#include "gfx/QualityGovernor.hpp"
#include "gfx/Relaunch.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <string_view>

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

// Average interval between presentations of a render loop paced by FramePacer: each frame takes `renderMs` to
// draw and, with VSync (`refreshHz` > 0), is presented on the next refresh after that.
double pacedIntervalMs(int cap, double refreshHz, double renderMs, int frames = 60)
{
    using Clock = std::chrono::steady_clock;
    using Ms = std::chrono::duration<double, std::milli>;
    const Clock::time_point origin = Clock::time_point{} + std::chrono::hours(1);
    const double vsyncMs = refreshHz > 0.0 ? 1000.0 / refreshHz : 0.0;
    double start = 0.0;
    double first = 0.0;
    double last = 0.0;
    FramePacer pacer;
    for (int frame = 0; frame < frames; ++frame) {
        const double done = start + renderMs;
        const double presented = vsyncMs > 0.0 ? std::ceil(done / vsyncMs) * vsyncMs : done;
        (frame == 0 ? first : last) = presented;
        start = presented;
        if (const auto wake = pacer.wakeUp(origin + std::chrono::duration_cast<Clock::duration>(Ms(presented)), cap)) {
            start = std::max(presented, Ms(*wake - origin).count());
        }
    }
    return (last - first) / (frames - 1);
}

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

    TEST_CASE("a window that starts hidden leaves the sentinel disarmed until it is first shown")
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("graphics-init.sentinel"));
        CrashSentinel sentinel(path);
        sentinel.arm();  // as the graphics bootstrap does
        QObject context;
        QQuickWindow window;
        window.resize(64, 64);
        int frames = 0;
        coverFirstFrame(sentinel, window, context, [&frames] { ++frames; });
        // In the tray: no graphics are initialised, so a kill now is no graphics crash.
        CHECK_FALSE(QFile::exists(path));
        test::waitUntil([] { return false; }, 100);
        CHECK_FALSE(QFile::exists(path));

        window.show();
        CHECK(QFile::exists(path));
        REQUIRE(test::waitUntil([&frames] { return frames == 1; }));
        CHECK_FALSE(QFile::exists(path));
        window.hide();
        window.show();
        test::waitUntil([] { return false; }, 100);
        CHECK_FALSE(QFile::exists(path));
        CHECK(frames == 1);
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

    TEST_CASE("deliberate low-rate redraws between animation frames are not slow frames")
    {
        // 0.6 s of a 60 Hz animation, then the backdrop drift's 12 Hz redraws (Backdrop.qml) in the same window.
        const double budget = 1000.0 / 60.0;
        std::vector<double> intervals(36, budget);
        intervals.insert(intervals.end(), {42.0, 83.0, 83.0, 83.0, 83.0, 83.0, 83.0, 83.0, 83.0, 83.0});
        FrameWindowStats stats;
        stats.frames = 47;
        stats.budgetMs = budget;
        stats.intervalP90Ms = frameIntervalP90(intervals, budget);
        CHECK(stats.intervalP90Ms == doctest::Approx(budget));
        QualityGovernor governor;
        CHECK_FALSE(governor.feed(stats, 1).has_value());
        CHECK_FALSE(governor.feed(stats, 2).has_value());
        CHECK(governor.tier() == Tier::High);

        // Missed frames in a continuous animation still count, at 60 Hz and at 144 Hz (36 fps).
        std::vector<double> dropped(30, budget);
        dropped.insert(dropped.end(), 10, 2.0 * budget);
        CHECK(frameIntervalP90(dropped, budget) == doctest::Approx(2.0 * budget));
        const double fast = 1000.0 / 144.0;
        CHECK(frameIntervalP90(std::vector<double>(36, 4.0 * fast), fast) == doctest::Approx(4.0 * fast));
        // With a 30 fps cap one missed frame counts, the drift's 83 ms redraws do not.
        const double capped = 1000.0 / 30.0;
        std::vector<double> cappedDrift(30, capped);
        cappedDrift.insert(cappedDrift.end(), 5, 83.3);
        CHECK(frameIntervalP90(cappedDrift, capped) == doctest::Approx(capped));
        std::vector<double> cappedDropped(30, capped);
        cappedDropped.insert(cappedDropped.end(), 5, 2.0 * capped);
        CHECK(frameIntervalP90(cappedDropped, capped) == doctest::Approx(2.0 * capped));
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

    TEST_CASE("the frame cap paces from the frame just presented")
    {
        // 30 fps on a 60 Hz display: every second refresh, not every one, also when a frame takes longer to
        // draw than the wake-up slack.
        CHECK(pacedIntervalMs(30, 60.0, 1.0) == doctest::Approx(1000.0 / 30).epsilon(0.01));
        CHECK(pacedIntervalMs(30, 60.0, 4.0) == doctest::Approx(1000.0 / 30).epsilon(0.01));
        // A cap that does not divide the refresh rate averages out at the cap.
        CHECK(pacedIntervalMs(60, 144.0, 1.0) == doctest::Approx(1000.0 / 60).epsilon(0.02));
        CHECK(pacedIntervalMs(60, 144.0, 1.0) >= 1000.0 / 60 - 0.01);
        // Without VSync, too.
        CHECK(pacedIntervalMs(60, 0.0, 2.0) == doctest::Approx(1000.0 / 60).epsilon(0.01));
        // Frames slower than the cap are not slowed down further.
        CHECK(pacedIntervalMs(60, 0.0, 25.0) == doctest::Approx(25.0).epsilon(0.02));
        // After an idle scene the grid restarts: the next frame waits a full period, no burst.
        FramePacer pacer;
        const auto start = std::chrono::steady_clock::now();
        (void)pacer.wakeUp(start, 60);
        const auto later = start + std::chrono::seconds(2);
        const auto wake = pacer.wakeUp(later, 60);
        REQUIRE(wake.has_value());
        CHECK(*wake - later >= std::chrono::milliseconds(15));
        // No cap: every refresh.
        CHECK(pacedIntervalMs(0, 60.0, 1.0) == doctest::Approx(1000.0 / 60).epsilon(0.01));
    }

    TEST_CASE("the interactive cap replaces the idle one for a moment after an input")
    {
        using std::chrono::milliseconds;
        CHECK(FramePacer::capFor(30, 0, milliseconds(0)) == 0);
        CHECK(FramePacer::capFor(30, 0, milliseconds(999)) == 0);
        CHECK(FramePacer::capFor(30, 0, milliseconds(1000)) == 30);
        CHECK(FramePacer::capFor(30, 60, milliseconds(10)) == 60);
    }
}

// Needs a threaded GPU window: QT_QPA_PLATFORM=wayland QT_QUICK_BACKEND=rhi noctara-tests --no-skip -tc="energy
// saving*"
TEST_CASE("energy saving input response benchmark" * doctest::skip())
{
    QQmlApplicationEngine engine;
    engine.loadData(R"(import QtQuick
        Window { width: 400; height: 300; visible: true
            Rectangle { width: 100; height: 100; color: "teal"; RotationAnimation on rotation { from: 0; to: 360; duration: 1000; loops: Animation.Infinite } }
        })");
    REQUIRE_FALSE(engine.rootObjects().isEmpty());
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    REQUIRE(window != nullptr);
    FrameMonitor monitor;
    monitor.attach(window);
    int frames = 0;
    QObject::connect(window, &QQuickWindow::frameSwapped, window, [&frames] { ++frames; }, Qt::QueuedConnection);
    QTest::qWait(1500);

    const auto measure = [&](int capFps, int interactiveFps, bool input) {
        monitor.setFrameCap(capFps, interactiveFps);
        QTest::qWait(1200);  // the previous phase's input has worn off
        frames = 0;
        QElapsedTimer clock;
        clock.start();
        double worstGapMs = 0;
        double total = 0;
        int samples = 0;
        qint64 last = clock.nsecsElapsed();
        while (clock.elapsed() < 2000) {
            if (input) {
                QMouseEvent move(QEvent::MouseMove, QPointF(50, 50), QPointF(50, 50), Qt::NoButton, Qt::NoButton,
                                 Qt::NoModifier);
                QCoreApplication::sendEvent(window, &move);
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
            QThread::msleep(4);  // a 250 Hz mouse
            const qint64 now = clock.nsecsElapsed();
            const double gapMs = static_cast<double>(now - last) / 1e6;
            worstGapMs = std::max(worstGapMs, gapMs);
            total += gapMs;
            ++samples;
            last = now;
        }
        MESSAGE("cap " << capFps << " interactive " << interactiveFps << (input ? " with input" : " no input") << ": "
                       << frames / 2.0 << " fps, exposed " << window->isExposed() << ", GUI loop avg "
                       << total / samples << " ms, worst " << worstGapMs << " ms");
    };
    measure(0, 0, false);
    measure(30, 30, true);  // what energy saving did before
    measure(30, 0, true);   // input lifts the cap
    measure(30, 0, false);  // nothing to do: the cap holds
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
        CHECK(state.interactiveCapFps == 0);  // energy saving never slows the user down

        inputs.frameRateCap = QStringLiteral("60");
        CHECK(resolveEffects(inputs).frameCapFps == 30);
        inputs.governorTier = Tier::Solid;
        CHECK(resolveEffects(inputs).styleCap == QStringLiteral("solid"));  // the stricter cap wins
        inputs.energySaving = false;
        inputs.governorTier = Tier::High;
        CHECK(resolveEffects(inputs).frameCapFps == 60);
        CHECK(resolveEffects(inputs).interactiveCapFps == 60);
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

TEST_SUITE("gfx.relaunch")
{
    TEST_CASE("a relaunch gets the environment the process started with, not the GPU variables it set")
    {
        // The GPU selection variables that bootstrapGraphics sets look like a user's choice to the new process.
        constexpr std::array kSet{"DRI_PRIME", "QT_VK_PHYSICAL_DEVICE_INDEX"};
        std::vector<std::optional<QByteArray>> previous;
        for (const char* name : kSet) {
            previous.push_back(qEnvironmentVariableIsSet(name) ? std::optional(qgetenv(name)) : std::nullopt);
            qunsetenv(name);
        }
        qputenv("NOCTARA_TEST_RELAUNCH", "kept");
        {
            const RelaunchGuard guard;
            for (const char* name : kSet) {
                qputenv(name, "1");
            }
            const std::vector<std::string> environment = guard.environment();
            const auto has = [&environment](std::string_view prefix) {
                return std::ranges::any_of(environment,
                                           [prefix](const std::string& entry) { return entry.starts_with(prefix); });
            };
            CHECK_FALSE(has("DRI_PRIME="));
            CHECK_FALSE(has("QT_VK_PHYSICAL_DEVICE_INDEX="));
            CHECK(has("NOCTARA_TEST_RELAUNCH=kept"));
        }
        qunsetenv("NOCTARA_TEST_RELAUNCH");
        for (std::size_t i = 0; i < kSet.size(); ++i) {
            if (previous[i]) {
                qputenv(kSet[i], *previous[i]);
            } else {
                qunsetenv(kSet[i]);
            }
        }
    }
}

namespace {

void writeFile(const QString& path, const QByteArray& content)
{
    REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(content);
}

}  // namespace

TEST_SUITE("gfx.fingerprint")
{
    TEST_CASE("a Mesa update changes the fingerprint, whatever the library directory")
    {
        // Mesa installs its gallium drivers as libgallium-<version>.so, with no unversioned name.
        const std::array<std::string, 3> libDirs{"/usr/lib", "/usr/lib64", "/usr/lib/x86_64-linux-gnu"};
        for (const std::string& libDir : libDirs) {
            CAPTURE(libDir);
            QTemporaryDir root;
            REQUIRE(root.isValid());
            const QString dir = root.path() + QString::fromStdString(libDir);
            writeFile(dir + QStringLiteral("/libgallium-26.1.0-arch1.1.so"), "gallium 26.1");
            const QString before = graphicsStackFingerprint(root.path());
            CHECK(graphicsStackFingerprint(root.path()) == before);
            REQUIRE(QFile::rename(dir + QStringLiteral("/libgallium-26.1.0-arch1.1.so"),
                                  dir + QStringLiteral("/libgallium-26.2.6-arch1.1.so")));
            CHECK(graphicsStackFingerprint(root.path()) != before);
        }
    }

    TEST_CASE("adding or removing a GPU changes the fingerprint")
    {
        QTemporaryDir root;
        REQUIRE(root.isValid());
        const QString drm = root.path() + QStringLiteral("/sys/class/drm");
        writeFile(drm + QStringLiteral("/card1/device/vendor"), "0x10de\n");
        writeFile(drm + QStringLiteral("/card1/device/device"), "0x1b81\n");
        REQUIRE(QDir().mkpath(drm + QStringLiteral("/card1-DP-1")));
        const QString single = graphicsStackFingerprint(root.path());

        writeFile(drm + QStringLiteral("/card2/device/vendor"), "0x1002\n");
        writeFile(drm + QStringLiteral("/card2/device/device"), "0x7480\n");
        const QString added = graphicsStackFingerprint(root.path());
        CHECK(added != single);

        REQUIRE(QDir(drm + QStringLiteral("/card2")).removeRecursively());
        CHECK(graphicsStackFingerprint(root.path()) == single);
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
