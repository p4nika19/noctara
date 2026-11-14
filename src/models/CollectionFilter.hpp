#pragma once

#include "models/CollectionModel.hpp"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

namespace noctara::models {

// A search within one list (likes, the library's playlists, a playlist's tracks): the rows whose title, artist,
// owner, album or genre contain every word typed, ignoring case and accents. `model` is the source itself while
// nothing is typed, otherwise a list of the matches that follows the source as it changes (a sync, tracks fetched
// later, a new like) row by row, so the view keeps its place. Playing from it queues the matches. A playlist's
// tracks known only by id are fetched once a search starts, so they can be found too.
class CollectionFilter : public QObject {
    Q_OBJECT
    Q_PROPERTY(noctara::models::CollectionModel* source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    // What the view shows: the source, or the matches while a search is on.
    Q_PROPERTY(noctara::models::CollectionModel* model READ model NOTIFY modelChanged)
    // A search is on (text has at least one word).
    Q_PROPERTY(bool active READ active NOTIFY modelChanged)

public:
    explicit CollectionFilter(QObject* parent = nullptr);

    [[nodiscard]] CollectionModel* source() const noexcept { return m_source; }
    void setSource(CollectionModel* source);
    [[nodiscard]] const QString& text() const noexcept { return m_text; }
    void setText(const QString& text);
    [[nodiscard]] CollectionModel* model() const noexcept;
    [[nodiscard]] bool active() const noexcept { return !m_words.isEmpty() && m_source != nullptr; }

    // Case- and accent-insensitive form of `text` ("Beyoncé" -> "beyonce", "Ёлка" -> "елка").
    [[nodiscard]] static QString fold(const QString& text);
    // The folded words of a search.
    [[nodiscard]] static QStringList words(const QString& text);
    // Every word occurs in the entry's title, artist, uploader or owner, album or genre (a user's name, full name
    // or city). Tracks known only by id never match.
    [[nodiscard]] static bool matches(const api::Entry& entry, const QStringList& words);

Q_SIGNALS:
    void sourceChanged();
    void textChanged();
    void modelChanged();

private:
    void refilter();
    void scheduleRefilter();
    void ensureModel();

    QPointer<CollectionModel> m_source;
    CollectionModel* m_matches = nullptr;  // child of this, made on the first search
    QString m_text;
    QStringList m_words;
    bool m_queued = false;
    bool m_hydrating = false;  // the source's stubs were asked for (once per source)
    QList<QMetaObject::Connection> m_connections;
};

}  // namespace noctara::models
