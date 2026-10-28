#pragma once

#include "models/CollectionModel.hpp"

#include <QObject>
#include <QString>
#include <QTimer>

namespace noctara::api {
class ApiService;
}

namespace noctara::ui {

// QML "Catalog" singleton: SoundCloud content for the pages. For now the search page: typing searches after a
// short pause (Enter searches at once), and the kind ("all", "tracks", "playlists", "albums", "users") narrows it.
class CatalogController : public QObject {
    Q_OBJECT
    Q_PROPERTY(noctara::models::CollectionModel* searchResults READ searchResults CONSTANT)
    Q_PROPERTY(QString query READ query NOTIFY queryChanged)
    Q_PROPERTY(QString searchKind READ searchKind NOTIFY queryChanged)

public:
    explicit CatalogController(api::ApiService& api, QObject* parent = nullptr);

    [[nodiscard]] models::CollectionModel* searchResults() noexcept { return &m_search; }
    [[nodiscard]] const QString& query() const noexcept { return m_query; }
    [[nodiscard]] const QString& searchKind() const noexcept { return m_kind; }

    // Debounced: the search starts once typing pauses.
    Q_INVOKABLE void search(const QString& text, const QString& kind);
    // Starts the pending search immediately (Enter).
    Q_INVOKABLE void searchNow();

    static constexpr std::chrono::milliseconds kTypingPause{350};

Q_SIGNALS:
    void queryChanged();

private:
    void start();

    api::ApiService& m_api;
    models::CollectionModel m_search;
    QString m_query;
    QString m_kind = QStringLiteral("all");
    QString m_started;  // query + kind of the search shown
    QTimer m_debounce;
};

}  // namespace noctara::ui
