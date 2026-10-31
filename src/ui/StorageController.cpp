#include "ui/StorageController.hpp"

#include "i18n/ErrorMessages.hpp"
#include "i18n/LanguageManager.hpp"
#include "settings/Keys.hpp"
#include "ui/SettingsController.hpp"

#include <QDir>
#include <QVariantMap>

#include <algorithm>

namespace noctara::ui {

StorageController::StorageController(cache::CacheService& service, i18n::LanguageManager& languages,
                                     SettingsController& settingsUi, QObject* parent)
    : QObject(parent)
    , m_service(service)
    , m_settingsUi(settingsUi)
{
    m_throttle.setSingleShot(true);
    m_throttle.setInterval(1000);
    connect(&m_throttle, &QTimer::timeout, this, [this] {
        if (std::exchange(m_refreshPending, false)) {
            refresh();
            m_throttle.start();
        }
    });
    // The first change shows at once; changes within the next second are folded into one more refresh.
    connect(&m_service, &cache::CacheService::usageChanged, this, [this] {
        if (m_throttle.isActive()) {
            m_refreshPending = true;
            return;
        }
        refresh();
        m_throttle.start();
    });
    connect(&m_service, &cache::CacheService::statusChanged, this, [this] {
        updateRowStatuses();
        Q_EMIT statusChanged();
    });
    connect(&languages, &i18n::LanguageManager::languageChanged, this, [this] {
        updateRowStatuses();
        Q_EMIT statusChanged();
    });
    updateRowStatuses();
    refresh();
}

QVariantList StorageController::segments() const
{
    std::vector<std::pair<QString, qint64>> parts{
        {QStringLiteral("tracks"), m_usage.audio.completeBytes},
        {QStringLiteral("partial"), m_usage.audio.partialBytes},
        {QStringLiteral("covers"), m_usage.audio.coverBytes},
        {QStringLiteral("images"), m_usage.images.diskBytes},
        {QStringLiteral("metadata"), m_usage.metadataBytes},
    };
    std::ranges::stable_sort(parts, [](const auto& a, const auto& b) { return a.second > b.second; });
    QVariantList list;
    for (const auto& [id, bytes] : parts) {
        list.append(QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("bytes"), bytes}});
    }
    return list;
}

qint64 StorageController::totalBytes() const
{
    return m_usage.audio.completeBytes + m_usage.audio.partialBytes + m_usage.audio.coverBytes +
           m_usage.images.diskBytes + m_usage.metadataBytes;
}

QString StorageController::location() const
{
    return QDir::toNativeSeparators(m_service.status().root);
}

QString StorageController::problem() const
{
    const cache::CacheStatus& status = m_service.status();
    if (status.problem) {
        if (*status.problem == ErrorCode::CacheLocked) {
            return i18n::errorMessage(ErrorCode::CacheLocked);
        }
        //: %1 = explanation of the error
        return tr("The cache folder cannot be used, so caches are kept in memory only. %1")
            .arg(i18n::errorMessage(*status.problem));
    }
    if (status.fellBack) {
        return tr("The chosen cache location cannot be used, so the standard folder is used instead.");
    }
    return {};
}

void StorageController::refresh()
{
    m_service.usage(this, [this](cache::CacheUsage usage) {
        m_usage = usage;
        Q_EMIT usageChanged();
    });
}

void StorageController::clear(const QString& part)
{
    using Part = cache::CacheService::Part;
    const Part which = part == u"audio"      ? Part::Audio
                       : part == u"images"   ? Part::Images
                       : part == u"metadata" ? Part::Metadata
                                             : Part::All;
    ++m_clearing;
    Q_EMIT clearingChanged();
    if (which == Part::All) {
        Q_EMIT clearingAll();
    }
    m_service.clear(which, this, [this, which] {
        --m_clearing;
        Q_EMIT clearingChanged();
        Q_EMIT notice(QStringLiteral("success"), which == Part::Audio      ? tr("Cached tracks removed.")
                                                 : which == Part::Images   ? tr("Cached images removed.")
                                                 : which == Part::Metadata ? tr("Cached metadata removed.")
                                                                           : tr("All caches cleared."));
        refresh();
    });
}

void StorageController::updateRowStatuses()
{
    const QString root = location();
    //: %1 = folder path
    m_settingsUi.setRowStatus(
        QString::fromLatin1(settings::keys::CacheDir.data(), static_cast<qsizetype>(settings::keys::CacheDir.size())),
        root.isEmpty() ? tr("Not in use: caches are kept in memory only.") : tr("In use: %1").arg(root));
}

}  // namespace noctara::ui
