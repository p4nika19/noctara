#pragma once

#include "cache/CacheService.hpp"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

namespace noctara::i18n {
class LanguageManager;
}

namespace noctara::ui {

class SettingsController;

// QML "Storage" singleton for the Storage & Cache page: how much each cache uses (per category, for the usage
// meter), where the caches live and why the disk caches might be off, and the Clear actions (the page asks
// for confirmation first). Sizes are formatted in QML with Format.bytes.
class StorageController : public QObject {
    Q_OBJECT
    // [{id: "tracks"|"partial"|"covers"|"images"|"metadata", bytes}], largest share first.
    Q_PROPERTY(QVariantList segments READ segments NOTIFY usageChanged)
    Q_PROPERTY(qint64 totalBytes READ totalBytes NOTIFY usageChanged)
    Q_PROPERTY(qint64 freeBytes READ freeBytes NOTIFY usageChanged)
    Q_PROPERTY(int cachedTracks READ cachedTracks NOTIFY usageChanged)
    Q_PROPERTY(qint64 memoryBytes READ memoryBytes NOTIFY usageChanged)
    Q_PROPERTY(qint64 memoryBudget READ memoryBudget NOTIFY usageChanged)
    Q_PROPERTY(bool diskFull READ diskFull NOTIFY usageChanged)
    Q_PROPERTY(QString location READ location NOTIFY statusChanged)
    Q_PROPERTY(QString problem READ problem NOTIFY statusChanged)
    Q_PROPERTY(bool clearing READ clearing NOTIFY clearingChanged)

public:
    StorageController(cache::CacheService& service, i18n::LanguageManager& languages, SettingsController& settingsUi,
                      QObject* parent = nullptr);

    [[nodiscard]] QVariantList segments() const;
    [[nodiscard]] qint64 totalBytes() const;
    [[nodiscard]] qint64 freeBytes() const noexcept { return m_usage.freeBytes; }
    [[nodiscard]] int cachedTracks() const noexcept { return m_usage.audio.completeEntries; }
    [[nodiscard]] qint64 memoryBytes() const noexcept { return m_usage.audioMemory + m_usage.images.memoryBytes; }
    [[nodiscard]] qint64 memoryBudget() const noexcept
    {
        return m_usage.audioMemoryBudget + m_usage.images.memoryBudget;
    }
    [[nodiscard]] bool diskFull() const noexcept { return m_usage.audio.diskStalled; }
    [[nodiscard]] QString location() const;
    [[nodiscard]] QString problem() const;
    [[nodiscard]] bool clearing() const noexcept { return m_clearing > 0; }

    Q_INVOKABLE void refresh();
    // part: "audio" | "images" | "metadata" | "all"
    Q_INVOKABLE void clear(const QString& part);

Q_SIGNALS:
    void usageChanged();
    void statusChanged();
    void clearingChanged();
    // kind: "success" | "warning" | "error"; for the page's toasts.
    void notice(const QString& kind, const QString& message);

private:
    void updateRowStatuses();

    cache::CacheService& m_service;
    SettingsController& m_settingsUi;
    cache::CacheUsage m_usage;
    QTimer m_throttle;  // usage is re-read at most once a second
    bool m_refreshPending = false;
    int m_clearing = 0;
};

}  // namespace noctara::ui
