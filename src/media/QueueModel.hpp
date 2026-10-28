#pragma once

#include <QAbstractListModel>

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
    };
    Q_ENUM(Role)

    explicit QueueModel(const PlaybackQueue& queue, QObject* parent = nullptr);

    // The queue changed structurally.
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
    const PlaybackQueue& m_queue;
};

}  // namespace noctara::media
