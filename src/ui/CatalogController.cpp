#include "ui/CatalogController.hpp"

#include "api/ApiService.hpp"

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

CatalogController::CatalogController(api::ApiService& api, QObject* parent)
    : QObject(parent)
    , m_api(api)
    , m_search(api.client())
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(kTypingPause);
    connect(&m_debounce, &QTimer::timeout, this, &CatalogController::start);
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
