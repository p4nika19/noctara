#pragma once

#include <QJsonObject>
#include <QRegularExpression>
#include <QString>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

// Pure decision logic for the graphics bootstrap: requested settings + probe results + blocklist
// -> hardware or software rendering. No Qt GUI dependency, fully unit-testable with mocked probes.
namespace noctara::gfx {

enum class Acceleration : std::uint8_t { Auto, On, Off };
enum class Api : std::uint8_t { Auto, OpenGL, Vulkan };
enum class DeviceKind : std::uint8_t { Unknown, Integrated, Discrete, Virtual, Cpu };

[[nodiscard]] Acceleration parseAcceleration(const QString& text) noexcept;
[[nodiscard]] Api parseApi(const QString& text) noexcept;
[[nodiscard]] QString apiName(Api api);
[[nodiscard]] QString deviceKindName(DeviceKind kind);

struct ProbeResult {
    bool ok = false;
    Api api = Api::Auto;
    QString deviceName;
    QString driver;
    std::uint64_t vendorId = 0;
    std::uint64_t deviceId = 0;
    DeviceKind kind = DeviceKind::Unknown;
    int adapterCount = 0;
    // Vulkan: the physical device chosen for the GPU preference (applied via QT_VK_PHYSICAL_DEVICE_INDEX);
    // -1 leaves the choice to Qt.
    int deviceIndex = -1;
    QString error;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static ProbeResult fromJson(const QJsonObject& json);
};

// Known software rasterizers are always detected by name, even if the driver reports a GPU type.
[[nodiscard]] bool isSoftwareRasterizer(const QString& deviceName);

enum class BlockAction : std::uint8_t { Software, AvoidApi, NoLiquidGlass };

struct BlocklistEntry {
    QString id;
    std::optional<std::uint64_t> vendorId;
    QRegularExpression deviceName;  // empty pattern = any
    QRegularExpression driver;
    Api api = Api::Auto;  // Auto = any API
    BlockAction action = BlockAction::Software;
    QString reason;  // English, logged
};

using Blocklist = std::vector<BlocklistEntry>;

// Parses the data file (resources/data/gpu-blocklist.json). Invalid entries are skipped with a log line.
[[nodiscard]] Blocklist parseBlocklist(const QByteArray& json);
[[nodiscard]] const BlocklistEntry* matchBlocklist(const Blocklist& list, const ProbeResult& probe);

enum class Reason : std::uint8_t {
    Hardware,            // GPU in use as requested
    UserDisabled,        // setting / --disable-gpu
    SafeModeFlag,        // --safe-mode
    CrashRecovery,       // previous launch crashed during graphics init
    ProbeFailed,         // no API could create a device
    SoftwareRasterizer,  // llvmpipe, SwiftShader, ...
    Blocklisted,         // known-bad driver
    ApiFallback,         // requested API failed, another one is used
};

struct GraphicsRequest {
    Acceleration acceleration = Acceleration::Auto;
    Api api = Api::Auto;
    bool safeModeFlag = false;
    bool crashSentinelFound = false;
};

struct GraphicsDecision {
    bool hardware = false;
    bool safeMode = false;
    Api api = Api::Auto;  // API in use when hardware == true
    Reason reason = Reason::Hardware;
    bool showNotice = false;  // one-time translated notice for the user
    bool liquidGlassAllowed = true;
    std::optional<ProbeResult> probe;  // the accepted (or last failed) probe
    QString detail;                    // English, logged
};

using ProbeFunction = std::function<ProbeResult(Api)>;

// APIs to try, in order: the requested one first, then the alternatives.
[[nodiscard]] std::vector<Api> candidateApis(Api requested);

[[nodiscard]] GraphicsDecision decide(const GraphicsRequest& request, const ProbeFunction& probe,
                                      const Blocklist& blocklist);

}  // namespace noctara::gfx
