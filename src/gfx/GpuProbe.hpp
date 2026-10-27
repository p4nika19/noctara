#pragma once

#include "gfx/GraphicsPolicy.hpp"

#include <QString>

#include <map>

namespace noctara::gfx {

// Creates a throw-away QRhi device for the given API and reports what it found. Requires a
// QGuiApplication; must run on the GUI thread before any window exists.
[[nodiscard]] ProbeResult probeRhi(Api api);

// Identifies the installed graphics stack cheaply (file stats, no driver loading) so cached probe
// results are invalidated when drivers, Qt or relevant environment variables change.
[[nodiscard]] QString graphicsStackFingerprint();

// Probe results cached between launches in <cache>/gpu-probe.json.
class ProbeCache {
public:
    explicit ProbeCache(QString path);

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
