#pragma once

#include "core/Error.hpp"
#include "drm/DrmHost.hpp"
#include "drm/HelperFiles.hpp"

#include <QObject>
#include <QThread>

#include <cstdint>
#include <optional>

namespace noctara::net {
class INetworkTransport;
}

namespace noctara::drm {

class InstallJob;

// Sets the protected player up by itself, under <data>/drm-helper: castlabs Electron and hls.js at the versions
// pinned in electron/helper-release.json, downloaded through Noctara's network stack (so its proxy and DNS
// settings apply), resumed with a Range request after an interruption and checked against their SHA-256;
// Electron is unpacked with libarchive and the helper app is written out of Noctara itself. The runtime
// (electron-<version>/) and the app (app-<hash>/) are separate folders, so a changed app does not download the
// runtime again; older folders are removed. The work runs on its own thread; this object lives on the GUI thread.
class HelperInstaller : public QObject {
    Q_OBJECT

public:
    enum class Phase : std::uint8_t { Idle, Downloading, Unpacking, Done, Failed };

    // `release`: what to install; the pinned one (helperRelease()) unless a test gives its own.
    HelperInstaller(net::INetworkTransport& network, QString dataDir, std::optional<HelperRelease> release = {},
                    QObject* parent = nullptr);
    ~HelperInstaller() override;
    HelperInstaller(const HelperInstaller&) = delete;
    HelperInstaller& operator=(const HelperInstaller&) = delete;
    HelperInstaller(HelperInstaller&&) = delete;
    HelperInstaller& operator=(HelperInstaller&&) = delete;

    // The installed helper when both folders are complete (a few stat calls).
    [[nodiscard]] std::optional<HelperCommand> installed() const;
    // Starts unless it is running already. `background`: refused on a metered connection (MeteredDeferred).
    void install(bool background);

    [[nodiscard]] Phase phase() const noexcept { return m_phase; }
    [[nodiscard]] bool running() const noexcept { return m_phase == Phase::Downloading || m_phase == Phase::Unpacking; }
    [[nodiscard]] std::int64_t received() const noexcept { return m_received; }
    [[nodiscard]] std::int64_t total() const noexcept { return m_total; }
    [[nodiscard]] const Error& error() const noexcept { return m_error; }
    [[nodiscard]] QString root() const { return m_root; }

    [[nodiscard]] const HelperRelease& release() const noexcept { return m_release; }
    [[nodiscard]] QString runtimeDir() const;
    [[nodiscard]] QString appDir() const;

Q_SIGNALS:
    void progressChanged();
    // Installed (or failed: see error()).
    void finished(bool ok);

private:
    friend class InstallJob;
    void onProgress(Phase phase, std::int64_t received, std::int64_t total);
    void onFinished(const std::optional<Error>& error);

    net::INetworkTransport& m_network;
    QString m_root;
    HelperRelease m_release;
    QThread m_thread;
    InstallJob* m_job = nullptr;  // owned; lives on m_thread while installing
    Phase m_phase = Phase::Idle;
    std::int64_t m_received = 0;
    std::int64_t m_total = 0;
    Error m_error;
};

// Unpacks a zip into `into` (paths stay inside it; permissions kept, so executables stay executable).
[[nodiscard]] Result<void> extractZip(const QString& zip, const QString& into);

}  // namespace noctara::drm
