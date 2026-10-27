#pragma once

#include "gfx/GraphicsPolicy.hpp"

#include <QString>

#include <cstdint>
#include <map>
#include <vector>

namespace noctara::gfx {

enum class DevicePreference : std::uint8_t { Auto, Integrated, Discrete };
[[nodiscard]] DevicePreference parseDevicePreference(const QString& text) noexcept;

// Picks the physical device for a preference from the enumerated device kinds; -1 = no match (Qt decides).
[[nodiscard]] int preferredDeviceIndex(const std::vector<DeviceKind>& devices, DevicePreference preference) noexcept;

// Creates a throw-away QRhi device for the given API and reports what it found. For Vulkan the GPU preference
// selects the physical device (QT_VK_PHYSICAL_DEVICE_INDEX, unless the user set it). Requires a
// QGuiApplication; must run on the GUI thread before any window exists.
[[nodiscard]] ProbeResult probeRhi(Api api, DevicePreference preference = DevicePreference::Auto);

// Identifies the installed graphics stack cheaply (file stats, no driver loading) so cached probe
// results are invalidated when drivers, Qt or relevant environment variables change.
[[nodiscard]] QString graphicsStackFingerprint();

// Probe results cached between launches in <cache>/gpu-probe.json.
class ProbeCache {
public:
    // `variant` distinguishes results that depend on settings (the GPU preference).
    explicit ProbeCache(QString path, const QString& variant = {});

    [[nodiscard]] std::optional<ProbeResult> lookup(Api api) const;
    void store(const ProbeResult& result);
    void save() const;

private:
    QString m_path;
    QString m_fingerprint;
    std::map<Api, ProbeResult> m_results;
    bool m_dirty = false;
};

}  // namespace noctara::gfx
