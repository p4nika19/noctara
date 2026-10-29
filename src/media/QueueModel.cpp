#include "media/QueueModel.hpp"

#include "media/PlaybackQueue.hpp"
#include "models/CollectionModel.hpp"

namespace noctara::media {

QueueModel::QueueModel(const PlaybackQueue& queue, QObject* parent)
    : QAbstractListModel(parent)
    , m_queue(queue)
{
}

void QueueModel::reload()
{
    beginResetModel();
    endResetModel();
    Q_EMIT countChanged();
    Q_EMIT currentChanged();
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
    return m_queue.size();
}

int QueueModel::currentIndex() const
{
    return m_queue.current();
}

int QueueModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_queue.size();
}

QVariant QueueModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_queue.size()) {
        return {};
    }
    const QueueItem& item = m_queue.items()[static_cast<std::size_t>(index.row())];
    const api::Track& track = item.track;
    switch (role) {
    case TitleRole: return track.title;
    case SubtitleRole: return track.artist();
    case ThumbnailRole:
        return track.artwork().isEmpty()
                   ? QString()
                   : api::sizedArtwork(track.artwork(), models::CollectionModel::kThumbnailPixels);
    case DurationRole: return QVariant::fromValue<qint64>(track.durationMs);
    case CurrentRole: return index.row() == m_queue.current();
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
