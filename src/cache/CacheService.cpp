#include "cache/CacheService.hpp"

#include "cache/FileOps.hpp"
#include "cache/HttpCache.hpp"
#include "cache/MemoryPressure.hpp"
#include "cache/Prefetch.hpp"
#include "core/Handoff.hpp"
#include "core/Log.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"

#include <QDir>
#include <QTemporaryDir>

namespace noctara::cache {

namespace {

constexpr std::int64_t kMiB = 1024LL * 1024;

}  // namespace

// Everything that lives on the cache thread.
class CacheCore : public QObject {
public:
    CacheCore(net::INetworkTransport& network, MemoryTier& memory, FileOps& ops)
        : http(network, ops, this)
        , audio(network, memory, ops, this)
        , images(network, ops, 64 * kMiB, this)
    {
        images.setFallback([this](const QUrl& url) { return audio.coverFor(url); });
        indexTimer.setSingleShot(true);
        // The disk index loads once the app is up, never during startup.
        indexTimer.setInterval(500);
        connect(&indexTimer, &QTimer::timeout, this, [this] { audio.loadIndex(); });
    }

    struct Config {
        std::optional<CacheLayout> layout;
        AudioCacheConfig audio;
        std::int64_t imageDisk = 0;
        std::int64_t metadataDisk = 0;
        bool layoutChanged = false;
    };

    void apply(const Config& config)
    {
        if (config.layoutChanged) {
            audio.setLayout(config.layout);
            indexTimer.start();
        }
        audio.setConfig(config.audio);
        const bool disk = config.layout.has_value();
        images.setDisk(disk && config.imageDisk > 0 ? config.layout->images() : QString(), config.imageDisk);
        http.setDisk(disk && config.metadataDisk > 0 ? config.layout->http() : QString(), config.metadataDisk);
    }

    CachingTransport http;
    AudioCache audio;
    ImageCache images;
    QTimer indexTimer;
    std::optional<CacheLock> lock;
};

CacheService::CacheService(settings::SettingsStore& settings, net::INetworkTransport& network, QString standardRoot,
                           QObject* parent, FileOps* ops)
    : QObject(parent)
    , m_settings(settings)
    , m_standardRoot(std::move(standardRoot))
    , m_memory(settings.intValue(settings::keys::AudioRamMb) * kMiB)
    , m_anchor(new QObject)  // NOLINT(cppcoreguidelines-owning-memory): deleted on the cache thread
    , m_pressure(std::make_unique<MemoryPressureMonitor>())
{
    m_thread.setObjectName(QStringLiteral("noctara-cache"));
    m_thread.start();
    m_anchor->moveToThread(&m_thread);
    FileOps& fileOps = ops != nullptr ? *ops : FileOps::posix();
    handoff::runAndWait(m_anchor, [&] {
        m_core = new CacheCore(network, m_memory, fileOps);  // NOLINT(cppcoreguidelines-owning-memory)
    });
    connect(&m_core->audio, &AudioCache::usageChanged, this, &CacheService::usageChanged);
    connect(m_pressure.get(), &MemoryPressureMonitor::pressure, this, [this] {
        // Keep what the current track needs; everything else in RAM can be fetched again.
        m_memory.trimTo(m_memory.stats().budget / 4);
        m_core->images.trimMemory(m_core->images.memoryBudget() / 4);
    });

    m_applyTimer.setSingleShot(true);
    m_applyTimer.setInterval(0);
    connect(&m_applyTimer, &QTimer::timeout, this, &CacheService::apply);
    connect(&m_settings, &settings::SettingsStore::valueChanged, this, [this](const QString& key) {
        if (key.startsWith(u"cache.")) {
            scheduleApply();
        }
    });
    apply();
}

CacheService::~CacheService()
{
    const bool clearOnExit = m_settings.boolValue(settings::keys::ClearCacheOnExit);
    handoff::runAndWait(m_anchor, [this, clearOnExit] {
        if (clearOnExit) {
            m_core->audio.clear();
            m_core->images.clear();
            m_core->http.clear();
        }
        delete std::exchange(m_core, nullptr);    // NOLINT(cppcoreguidelines-owning-memory)
        delete std::exchange(m_anchor, nullptr);  // NOLINT(cppcoreguidelines-owning-memory)
    });
    m_thread.quit();
    m_thread.wait();
}

net::INetworkTransport& CacheService::transport() noexcept
{
    return m_core->http;  // send() is thread-safe
}

AudioCache& CacheService::audio() noexcept
{
    return m_core->audio;
}

ImageCache& CacheService::images() noexcept
{
    return m_core->images;
}

QString CacheService::privateRoot()
{
    if (diskAvailable()) {
        return m_status.root;
    }
    if (!m_privateRoot) {
        m_privateRoot = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/noctara-XXXXXX"));
        if (!m_privateRoot->isValid()) {
            qCWarning(lcCache).noquote() << "no private directory for files kept beside the caches:"
                                         << m_privateRoot->errorString();
        }
    }
    return m_privateRoot->isValid() ? m_privateRoot->path() : QString();
}

bool CacheService::audioDiskEnabled() const
{
    return m_settings.boolValue(settings::keys::AudioCacheEnabled) &&
           m_settings.intValue(settings::keys::AudioDiskMb) > 0 && diskAvailable();
}

int CacheService::prefetchCount(bool metered) const
{
    return effectivePrefetchCount(static_cast<int>(m_settings.intValue(settings::keys::Prefetch)), metered,
                                  audioDiskEnabled());
}

void CacheService::runOnCacheThread(const std::function<void()>& function)
{
    handoff::runAndWait(m_anchor, function);
}

void CacheService::scheduleApply()
{
    m_applyTimer.start();
}

void CacheService::apply()
{
    const QString custom = m_settings.stringValue(settings::keys::CacheDir);
    const QString wanted = custom.isEmpty() ? m_standardRoot : custom + QStringLiteral("/noctara");
    const bool layoutChanged = wanted != m_appliedRoot;
    CacheStatus status = m_status;
    CacheCore::Config config;
    config.layoutChanged = layoutChanged;
    if (layoutChanged) {
        m_appliedRoot = wanted;
        status = CacheStatus{};
        status.custom = !custom.isEmpty();
        // Layout, lock and the directory checks touch the disk: done on the cache thread.
        handoff::runAndWait(m_anchor, [&] {
            m_core->lock.reset();
            const auto prepare = [&](const QString& root) -> Result<CacheLayout> {
                Result<CacheLayout> layout = prepareLayout(root);
                if (!layout) {
                    return layout;
                }
                Result<CacheLock> lock = CacheLock::acquire(*layout);
                if (!lock) {
                    return std::unexpected(lock.error());
                }
                m_core->lock = std::move(*lock);
                return layout;
            };
            Result<CacheLayout> layout = prepare(wanted);
            if (!layout && status.custom && layout.error().code != ErrorCode::CacheLocked) {
                qCWarning(lcCache).noquote()
                    << "cache location" << wanted << "is unusable ("
                    << QString::fromStdString(layout.error().describe()) << "), using the standard one";
                status.fellBack = true;
                layout = prepare(m_standardRoot);
            }
            if (layout) {
                status.root = layout->root;
                config.layout = *layout;
            } else {
                status.problem = layout.error().code;
                qCInfo(lcCache).noquote() << "disk caches off:" << QString::fromStdString(layout.error().describe());
            }
        });
    } else if (diskAvailable()) {
        config.layout = CacheLayout{m_status.root};
    }
    const bool audioEnabled = m_settings.boolValue(settings::keys::AudioCacheEnabled);
    const std::int64_t audioDisk = m_settings.intValue(settings::keys::AudioDiskMb) * kMiB;
    config.audio.diskEnabled = audioEnabled && audioDisk > 0 && config.layout.has_value();
    config.audio.diskBudget = audioDisk;
    config.audio.minFreeBytes = m_settings.intValue(settings::keys::MinFreeMb) * kMiB;
    config.audio.embedTags = m_settings.boolValue(settings::keys::EmbedTags);
    config.imageDisk = m_settings.intValue(settings::keys::ImageDiskMb) * kMiB;
    config.metadataDisk = m_settings.intValue(settings::keys::MetadataDiskMb) * kMiB;
    m_memory.setBudget(m_settings.intValue(settings::keys::AudioRamMb) * kMiB);
    m_core->images.setMemoryBudget(m_settings.intValue(settings::keys::ImageRamMb) * kMiB);
    handoff::post(m_anchor, [core = m_core, config] { core->apply(config); });
    if (layoutChanged) {
        m_status = std::move(status);
        Q_EMIT statusChanged();
    }
    Q_EMIT usageChanged();
}

void CacheService::usage(QObject* context, std::function<void(CacheUsage)> done)
{
    const auto channel = std::make_shared<net::ChannelPtr>(
        net::makeChannel(context, std::make_shared<std::atomic<bool>>(false), &m_thread));
    handoff::post(m_anchor, [core = m_core, channel, done = std::move(done), memory = &m_memory, root = m_status.root] {
        CacheUsage usage;
        usage.freeBytes = root.isEmpty() ? -1 : freeDiskSpace(root);
        usage.audio = core->audio.usage();
        const MemoryTier::Stats stats = memory->stats();
        usage.audioMemory = stats.bytes;
        usage.audioMemoryBudget = stats.budget;
        usage.images = core->images.usage();
        usage.metadataBytes = core->http.diskBytes();
        (*channel)->post([done, usage] { done(usage); });
        channel->reset();
    });
}

void CacheService::clear(Part part, QObject* context, std::function<void()> done)
{
    const auto channel = std::make_shared<net::ChannelPtr>(
        net::makeChannel(context, std::make_shared<std::atomic<bool>>(false), &m_thread));
    handoff::post(m_anchor, [core = m_core, part, channel, done = std::move(done)] {
        if (part == Part::Audio || part == Part::All) {
            core->audio.clear();
        }
        if (part == Part::Images || part == Part::All) {
            core->images.clear();
        }
        if (part == Part::Metadata || part == Part::All) {
            core->http.clear();
        }
        (*channel)->post([done] { done(); });
        channel->reset();
    });
}

}  // namespace noctara::cache
