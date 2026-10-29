#include "ui/CatalogController.hpp"

#include "api/ApiService.hpp"

#include "ui/AccountController.hpp"

#include <QCoreApplication>
#include <QQmlEngine>

namespace noctara::ui {

namespace {

api::endpoints::SearchKind toSearchKind(const QString& kind)
{
    using api::endpoints::SearchKind;
    if (kind == u"tracks") {
        return SearchKind::Tracks;
    }
    if (kind == u"playlists") {
        return SearchKind::Playlists;
    }
    if (kind == u"albums") {
        return SearchKind::Albums;
    }
    if (kind == u"users") {
        return SearchKind::Users;
    }
    return SearchKind::All;
}

}  // namespace

struct Genre {
    const char* id;
    const char* label;
};

// The genres SoundCloud keeps trending playlists for (soundcloud:system-playlists:trending-by-genre:<id>). Labels
// are ours, so they are translated.
constexpr Genre kGenres[] = {
    {"all-genres", QT_TRANSLATE_NOOP("Genres", "All genres")},
    {"afrobeat", QT_TRANSLATE_NOOP("Genres", "Afrobeat")},
    {"alternative-hip-hop", QT_TRANSLATE_NOOP("Genres", "Alternative Hip-hop")},
    {"alternative-rock", QT_TRANSLATE_NOOP("Genres", "Alternative Rock")},
    {"ambient", QT_TRANSLATE_NOOP("Genres", "Ambient")},
    {"dancehall", QT_TRANSLATE_NOOP("Genres", "Dancehall")},
    {"dubstep", QT_TRANSLATE_NOOP("Genres", "Dubstep")},
    {"electronic", QT_TRANSLATE_NOOP("Genres", "Electronic")},
    {"folk", QT_TRANSLATE_NOOP("Genres", "Folk")},
    {"hardcore", QT_TRANSLATE_NOOP("Genres", "Hardcore")},
    {"hip-hop", QT_TRANSLATE_NOOP("Genres", "Hip-hop")},
    {"house", QT_TRANSLATE_NOOP("Genres", "House")},
    {"indie", QT_TRANSLATE_NOOP("Genres", "Indie")},
    {"latin", QT_TRANSLATE_NOOP("Genres", "Latin")},
    {"metal", QT_TRANSLATE_NOOP("Genres", "Metal")},
    {"pop", QT_TRANSLATE_NOOP("Genres", "Pop")},
    {"r-n-b", QT_TRANSLATE_NOOP("Genres", "R&B")},
    {"reggae", QT_TRANSLATE_NOOP("Genres", "Reggae")},
    {"rock", QT_TRANSLATE_NOOP("Genres", "Rock")},
    {"trap", QT_TRANSLATE_NOOP("Genres", "Trap")},
};

CatalogController::CatalogController(api::ApiService& api, AccountController& account, QObject* parent)
    : QObject(parent)
    , m_api(api)
    , m_account(account)
    , m_search(api.client())
    , m_feed(api.client())
    , m_likes(api.client())
    , m_history(api.client())
    , m_library(api.client())
    , m_following(api.client())
    , m_charts(api.client())
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(kTypingPause);
    connect(&m_debounce, &QTimer::timeout, this, &CatalogController::start);
    connect(&m_account, &AccountController::changed, this, &CatalogController::accountChanged);
}

QVariantList CatalogController::genres() const
{
    QVariantList out;
    for (const Genre& genre : kGenres) {
        out.append(QVariantMap{{QStringLiteral("id"), QString::fromLatin1(genre.id)},
                               {QStringLiteral("label"), QCoreApplication::translate("Genres", genre.label)}});
    }
    return out;
}

void CatalogController::accountChanged()
{
    const qint64 id = m_account.userId();
    if (id == m_accountId) {
        return;
    }
    const qint64 previous = m_accountId;
    m_accountId = id;
    if (previous <= 0 && id > 0) {
        // The signed-in account became known (/me answered): lists loaded so far already belong to it. Likes and
        // following, which need the id, load when their page asks again.
        return;
    }
    // Personal lists belong to the account: reload those already shown, forget the others.
    const QStringList shown = m_loaded;
    m_loaded.clear();
    for (models::CollectionModel* model : {&m_feed, &m_likes, &m_history, &m_library, &m_following}) {
        model->clear();
    }
    for (const QString& list : shown) {
        if (list != u"charts") {
            ensure(list);
        } else {
            m_loaded.append(list);
        }
    }
}

void CatalogController::ensure(const QString& list)
{
    if (m_loaded.contains(list)) {
        return;
    }
    load(list);
}

void CatalogController::load(const QString& list)
{
    const api::Id me = m_account.userId();
    const int page = m_api.pageSize();
    if (list == u"charts") {
        m_charts.clear();
        m_charts.setSource([genre = m_chartGenre](int /*limit*/) { return api::endpoints::trending(genre); }, page);
    } else if (list == u"feed") {
        m_feed.setSource([](int limit) { return api::endpoints::stream(limit); }, page);
    } else if (list == u"history") {
        m_history.setSource([](int limit) { return api::endpoints::history(limit); }, page);
    } else if (list == u"library") {
        m_library.setSource([](int limit) { return api::endpoints::library(limit); }, page);
    } else if (list == u"likes" || list == u"following") {
        if (me == 0) {
            return;  // known once /me answered; accountChanged() loads it then
        }
        models::CollectionModel& model = list == u"likes" ? m_likes : m_following;
        model.setSource(
            [me, likes = list == u"likes"](int limit) {
                return likes ? api::endpoints::userLikes(me, limit) : api::endpoints::followings(me, limit);
            },
            page);
    } else {
        return;
    }
    m_loaded.append(list);
}

void CatalogController::setChartGenre(const QString& genre)
{
    if (genre == m_chartGenre) {
        return;
    }
    m_chartGenre = genre.isEmpty() ? QStringLiteral("all-genres") : genre;
    Q_EMIT chartChanged();
    m_loaded.removeAll(QStringLiteral("charts"));
    load(QStringLiteral("charts"));
}

template <class Detail> void CatalogController::keep(QHash<qint64, std::shared_ptr<Detail>>& cache, qint64 id)
{
    QList<qint64>& recent = std::is_same_v<Detail, models::PlaylistDetail> ? m_recentPlaylists : m_recentUsers;
    recent.removeAll(id);
    recent.append(id);
    while (recent.size() > kKeptDetails) {
        // The deleter uses deleteLater(): QML may still hold the oldest page until the event loop runs.
        cache.remove(recent.takeFirst());
    }
}

models::PlaylistDetail* CatalogController::playlist(qint64 id)
{
    auto it = m_playlists.find(id);
    if (it == m_playlists.end()) {
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): owned by the shared_ptr, deleted via deleteLater()
        auto detail = std::shared_ptr<models::PlaylistDetail>(new models::PlaylistDetail(m_api.client(), id),
                                                              [](models::PlaylistDetail* d) { d->deleteLater(); });
        QQmlEngine::setObjectOwnership(detail.get(), QQmlEngine::CppOwnership);
        it = m_playlists.insert(id, std::move(detail));
    }
    models::PlaylistDetail* detail = it->get();
    keep(m_playlists, id);
    return detail;
}

models::UserDetail* CatalogController::user(qint64 id)
{
    auto it = m_users.find(id);
    if (it == m_users.end()) {
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): owned by the shared_ptr, deleted via deleteLater()
        auto detail = std::shared_ptr<models::UserDetail>(new models::UserDetail(m_api.client(), id, m_api.pageSize()),
                                                          [](models::UserDetail* d) { d->deleteLater(); });
        QQmlEngine::setObjectOwnership(detail.get(), QQmlEngine::CppOwnership);
        it = m_users.insert(id, std::move(detail));
    }
    models::UserDetail* detail = it->get();
    keep(m_users, id);
    return detail;
}

void CatalogController::search(const QString& text, const QString& kind)
{
    const QString query = text.simplified();
    if (query == m_query && kind == m_kind) {
        return;
    }
    const bool kindChanged = kind != m_kind;
    m_query = query;
    m_kind = kind;
    Q_EMIT queryChanged();
    if (m_query.isEmpty()) {
        m_debounce.stop();
        m_started.clear();
        m_search.clear();
        return;
    }
    // Switching the kind is a deliberate click: no need to wait.
    if (kindChanged) {
        searchNow();
    } else {
        m_debounce.start();
    }
}

void CatalogController::searchNow()
{
    m_debounce.stop();
    start();
}

void CatalogController::start()
{
    const QString key = m_kind + u'\n' + m_query;
    if (m_query.isEmpty() || key == m_started) {
        return;
    }
    m_started = key;
    m_search.clear();
    m_search.setSource([query = m_query,
                        kind = toSearchKind(m_kind)](int limit) { return api::endpoints::search(query, kind, limit); },
                       m_api.pageSize());
}

}  // namespace noctara::ui
