#pragma once

#include "cache/AudioCache.hpp"
#include "cache/CacheLayout.hpp"
#include "cache/ImageCache.hpp"
#include "cache/MemoryTier.hpp"
#include "core/Error.hpp"
#include "net/Transport.hpp"

#include <QObject>
#include <QThread>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>

class QTemporaryDir;

namespace noctara::settings {
class SettingsStore;
}

namespace noctara::cache {

class CacheCore;
class FileOps;
class MemoryPressureMonitor;

struct CacheUsage {
    AudioUsage audio;
    std::int64_t audioMemory = 0;
    std::int64_t audioMemoryBudget = 0;
    ImageUsage images;
    std::int64_t metadataBytes = 0;
    std::int64_t freeBytes = -1;  // on the cache's file system
};

struct CacheStatus {
    QString root;                      // where the caches live (empty: nowhere, RAM only)
    bool custom = false;               // a user-chosen location
    std::optional<ErrorCode> problem;  // why the disk tiers are off (not writable, locked by another instance)
    bool fellBack = false;             // the custom location failed; the standard one is used instead
};

// Owns the caches on their own thread and keeps them in step with the cache.* settings (live): the audio
// RAM and disk tiers, the image cache, and the HTTP cache wrapped around the network transport. Lives on the
// GUI thread; transport() and the caches' thread-safe entry points may be used from any thread.
class CacheService : public QObject {
    Q_OBJECT

public:
    enum class Part : std::uint8_t { Audio, Images, Metadata, All };

    CacheService(settings::SettingsStore& settings, net::INetworkTransport& network, QString standardRoot,
                 QObject* parent = nullptr, FileOps* ops = nullptr);
    ~CacheService() override;
    CacheService(const CacheService&) = delete;
    CacheService& operator=(const CacheService&) = delete;
    CacheService(CacheService&&) = delete;
    CacheService& operator=(CacheService&&) = delete;

    // The HTTP-caching transport: what the API client uses.
    [[nodiscard]] net::INetworkTransport& transport() noexcept;
    [[nodiscard]] AudioCache& audio() noexcept;
    [[nodiscard]] ImageCache& images() noexcept;
    [[nodiscard]] MemoryTier& memory() noexcept { return m_memory; }
    [[nodiscard]] const CacheStatus& status() const noexcept { return m_status; }
    // Upcoming queue items worth warming right now.
    [[nodiscard]] int prefetchCount(bool metered) const;
    [[nodiscard]] bool diskAvailable() const noexcept { return !m_status.root.isEmpty() && !m_status.problem; }
    // Where other modules keep their own disposable files (the saved lists, the protected-playback helper's
    // profile): the cache root while this instance holds its lock, otherwise a private temporary directory
    // removed together with the service. Never a root another instance has locked.
    [[nodiscard]] QString privateRoot();

    void usage(QObject* context, std::function<void(CacheUsage)> done);
    void clear(Part part, QObject* context, std::function<void()> done);
    // Runs `function` on the cache thread and waits for it (tests, shutdown).
    void runOnCacheThread(const std::function<void()>& function);

Q_SIGNALS:
    void statusChanged();
    void usageChanged();

private:
    void scheduleApply();
    void apply();

    settings::SettingsStore& m_settings;
    QString m_standardRoot;
    MemoryTier m_memory;
    QThread m_thread;
    QObject* m_anchor = nullptr;  // lives on m_thread
    CacheCore* m_core = nullptr;  // lives on m_thread; created and destroyed there
    std::unique_ptr<MemoryPressureMonitor> m_pressure;
    CacheStatus m_status;
    QString m_appliedRoot;
    QTimer m_applyTimer;
    std::unique_ptr<QTemporaryDir> m_privateRoot;  // only when the disk tiers are off
};

}  // namespace noctara::cache
