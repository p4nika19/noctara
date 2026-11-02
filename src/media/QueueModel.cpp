#include "media/QueueModel.hpp"

#include "media/PlaybackQueue.hpp"
#include "models/CollectionModel.hpp"

#include <QSet>

#include <algorithm>
#include <iterator>

namespace noctara::media {

QueueModel::QueueModel(const PlaybackQueue& queue, QObject* parent)
    : QAbstractListModel(parent)
    , m_queue(queue)
{
}

void QueueModel::reload()
{
    std::vector<std::uint64_t> fresh;
    fresh.reserve(static_cast<std::size_t>(m_queue.size()));
    m_index.clear();
    for (const QueueItem& item : m_queue.items()) {
        m_index.insert(item.uid, static_cast<int>(fresh.size()));
        fresh.push_back(item.uid);
    }
    if (applyEdits(fresh)) {
        if (rowCount() > 0) {
            Q_EMIT dataChanged(index(0), index(rowCount() - 1));
        }
    } else {
        beginResetModel();
        m_rows = std::move(fresh);
        endResetModel();
    }
    Q_EMIT countChanged();
    Q_EMIT currentChanged();
}

bool QueueModel::applyEdits(const std::vector<std::uint64_t>& fresh)
{
    // The items both lists share, in the order shown and in the new order.
    std::vector<std::uint64_t> kept;
    std::ranges::copy_if(m_rows, std::back_inserter(kept), [this](std::uint64_t uid) { return m_index.contains(uid); });
    if (kept.empty()) {
        return false;  // a new queue
    }
    const QSet<std::uint64_t> shown(m_rows.begin(), m_rows.end());
    std::vector<std::uint64_t> target;
    std::ranges::copy_if(fresh, std::back_inserter(target),
                         [&shown](std::uint64_t uid) { return shown.contains(uid); });
    // At most one shared item moved ("Play next", a drag); more is a reshuffle.
    int moveFrom = -1;
    int moveTo = -1;
    if (kept != target) {
        const auto size = std::ssize(kept);
        std::ptrdiff_t first = 0;
        while (kept[static_cast<std::size_t>(first)] == target[static_cast<std::size_t>(first)]) {
            ++first;
        }
        std::ptrdiff_t last = size - 1;
        while (kept[static_cast<std::size_t>(last)] == target[static_cast<std::size_t>(last)]) {
            --last;
        }
        if (kept[static_cast<std::size_t>(first)] == target[static_cast<std::size_t>(last)] &&
            std::equal(kept.begin() + first + 1, kept.begin() + last + 1, target.begin() + first)) {
            moveFrom = static_cast<int>(first);
            moveTo = static_cast<int>(last);
        } else if (kept[static_cast<std::size_t>(last)] == target[static_cast<std::size_t>(first)] &&
                   std::equal(kept.begin() + first, kept.begin() + last, target.begin() + first + 1)) {
            moveFrom = static_cast<int>(last);
            moveTo = static_cast<int>(first);
        } else {
            return false;
        }
    }
    // 1. Rows that are gone, in contiguous blocks from the bottom.
    for (int row = rowCount() - 1; row >= 0;) {
        if (m_index.contains(m_rows[static_cast<std::size_t>(row)])) {
            --row;
            continue;
        }
        int first = row;
        while (first > 0 && !m_index.contains(m_rows[static_cast<std::size_t>(first - 1)])) {
            --first;
        }
        beginRemoveRows({}, first, row);
        m_rows.erase(m_rows.begin() + first, m_rows.begin() + row + 1);
        endRemoveRows();
        row = first - 1;
    }
    // 2. The moved row (the rows left are `kept` now).
    if (moveFrom >= 0) {
        const auto begin = m_rows.begin();
        if (moveFrom < moveTo) {
            beginMoveRows({}, moveFrom, moveFrom, {}, moveTo + 1);
            std::rotate(begin + moveFrom, begin + moveFrom + 1, begin + moveTo + 1);
        } else {
            beginMoveRows({}, moveFrom, moveFrom, {}, moveTo);
            std::rotate(begin + moveTo, begin + moveFrom, begin + moveFrom + 1);
        }
        endMoveRows();
    }
    // 3. New items, inserted in runs while walking the new order.
    std::size_t row = 0;
    for (std::size_t i = 0; i < fresh.size();) {
        if (row < m_rows.size() && m_rows[row] == fresh[i]) {
            ++row;
            ++i;
            continue;
        }
        std::size_t end = i;
        while (end < fresh.size() && (row >= m_rows.size() || m_rows[row] != fresh[end])) {
            ++end;
        }
        const auto first = static_cast<int>(row);
        beginInsertRows({}, first, first + static_cast<int>(end - i) - 1);
        m_rows.insert(m_rows.begin() + static_cast<std::ptrdiff_t>(row), fresh.begin() + static_cast<std::ptrdiff_t>(i),
                      fresh.begin() + static_cast<std::ptrdiff_t>(end));
        endInsertRows();
        row += end - i;
        i = end;
    }
    return true;
}

void QueueModel::refresh()
{
    if (rowCount() > 0) {
        Q_EMIT dataChanged(index(0), index(rowCount() - 1));
    }
}

void QueueModel::currentMoved(int previous)
{
    for (const int row : {previous, m_queue.current()}) {
        if (row >= 0 && row < rowCount()) {
            Q_EMIT dataChanged(index(row), index(row), {CurrentRole});
        }
    }
    Q_EMIT currentChanged();
}

int QueueModel::count() const
{
    return rowCount();
}

int QueueModel::currentIndex() const
{
    return m_queue.current();
}

int QueueModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant QueueModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= rowCount()) {
        return {};
    }
    // Between the steps of reload() rows and queue positions differ; every row shown is still queued.
    const int position = m_index.value(m_rows[static_cast<std::size_t>(index.row())], -1);
    if (position < 0 || position >= m_queue.size()) {
        return {};
    }
    const QueueItem& item = m_queue.items()[static_cast<std::size_t>(position)];
    const api::Track& track = item.track;
    switch (role) {
    case TitleRole: return track.title;
    case SubtitleRole: return track.artist();
    case ThumbnailRole:
        return track.artwork().isEmpty()
                   ? QString()
                   : api::sizedArtwork(track.artwork(), models::CollectionModel::kThumbnailPixels);
    case DurationRole: return QVariant::fromValue<qint64>(track.durationMs);
    case CurrentRole: return position == m_queue.current();
    case ProtectedRole: return track.drm();
    case PreviewRole: return track.previewOnly();
    case PendingRole: return track.stub;
    case UidRole: return QVariant::fromValue<quint64>(item.uid);
    case PermalinkRole: return track.permalinkUrl;
    case ArtistIdRole: return QVariant::fromValue<qint64>(track.user.id);
    default: return {};
    }
}

QHash<int, QByteArray> QueueModel::roleNames() const
{
    return {
        {TitleRole, "title"},         {SubtitleRole, "subtitle"}, {ThumbnailRole, "thumbnail"},
        {DurationRole, "duration"},   {CurrentRole, "current"},   {ProtectedRole, "protectedMedia"},
        {PreviewRole, "preview"},     {PendingRole, "pending"},   {UidRole, "uid"},
        {PermalinkRole, "permalink"}, {ArtistIdRole, "artistId"},
    };
}

}  // namespace noctara::media
