#pragma once

#include <QAbstractListModel>
#include <QHash>

#include <cstdint>
#include <vector>

namespace noctara::media {

class PlaybackQueue;

// The play queue for QML (the queue drawer): every item in playing order, the current one flagged.
class QueueModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentChanged)

public:
    enum Role {
        TitleRole = Qt::UserRole + 1,
        SubtitleRole,
        ThumbnailRole,
        DurationRole,
        CurrentRole,
        ProtectedRole,
        PreviewRole,
        PendingRole,  // a track known only by id, fetched before it plays
        UidRole,
        PermalinkRole,
        ArtistIdRole,
    };
    Q_ENUM(Role)

    explicit QueueModel(const PlaybackQueue& queue, QObject* parent = nullptr);

    // The queue changed structurally. Removed, inserted and single moved items are announced row by row, so views
    // keep their scroll position; a new queue or a reshuffle resets the model.
    void reload();
    // Items changed in place (hydrated tracks).
    void refresh();
    // Only the current item changed.
    void currentMoved(int previous);

    [[nodiscard]] int count() const;
    [[nodiscard]] int currentIndex() const;
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

Q_SIGNALS:
    void countChanged();
    void currentChanged();

private:
    // Turns the rows shown into `fresh` through row removals, insertions and at most one move. False (nothing
    // changed): the edit is something else, or nothing is shared.
    bool applyEdits(const std::vector<std::uint64_t>& fresh);

    const PlaybackQueue& m_queue;
    std::vector<std::uint64_t> m_rows;  // the uids the model shows, in row order (the queue after reload())
    QHash<std::uint64_t, int> m_index;  // uid -> position in the queue
};

}  // namespace noctara::media
