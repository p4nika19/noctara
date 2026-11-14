#include "models/CollectionFilter.hpp"

#include <QMetaObject>

#include <algorithm>

namespace noctara::models {

namespace {

bool hasAll(const QString& haystack, const QStringList& words)
{
    return std::ranges::all_of(words, [&haystack](const QString& word) { return haystack.contains(word); });
}

}  // namespace

CollectionFilter::CollectionFilter(QObject* parent)
    : QObject(parent)
{
}

QString CollectionFilter::fold(const QString& text)
{
    const QString decomposed = text.normalized(QString::NormalizationForm_KD);
    QString folded;
    folded.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        const QChar::Category category = c.category();
        if (category != QChar::Mark_NonSpacing && category != QChar::Mark_SpacingCombining &&
            category != QChar::Mark_Enclosing) {
            folded.append(c);
        }
    }
    return folded.toCaseFolded();
}

QStringList CollectionFilter::words(const QString& text)
{
    return fold(text).split(u' ', Qt::SkipEmptyParts);
}

bool CollectionFilter::matches(const api::Entry& entry, const QStringList& words)
{
    if (words.isEmpty()) {
        return true;
    }
    const QChar gap(u'\n');  // a word never spans two fields
    QString haystack;
    if (const auto* track = std::get_if<api::Track>(&entry.item)) {
        if (track->stub) {
            return false;
        }
        haystack = track->title + gap + track->artist() + gap + track->user.username + gap + track->publisherAlbum +
                   gap + track->genre;
    } else if (const auto* playlist = std::get_if<api::Playlist>(&entry.item)) {
        haystack = playlist->title + gap + playlist->user.username + gap + playlist->genre;
    } else if (const auto* user = std::get_if<api::User>(&entry.item)) {
        haystack = user->username + gap + user->fullName + gap + user->city;
    }
    return hasAll(fold(haystack), words);
}

CollectionModel* CollectionFilter::model() const noexcept
{
    return active() && m_matches != nullptr ? m_matches : m_source.data();
}

void CollectionFilter::setSource(CollectionModel* source)
{
    if (source == m_source) {
        return;
    }
    for (const QMetaObject::Connection& connection : std::as_const(m_connections)) {
        disconnect(connection);
    }
    m_connections.clear();
    m_source = source;
    m_hydrating = false;
    if (m_source) {
        const auto changed = [this] {
            scheduleRefilter();
        };
        m_connections = {
            connect(m_source, &QAbstractItemModel::dataChanged, this, changed),
            connect(m_source, &QAbstractItemModel::rowsInserted, this, changed),
            connect(m_source, &QAbstractItemModel::rowsRemoved, this, changed),
            connect(m_source, &QAbstractItemModel::rowsMoved, this, changed),
            connect(m_source, &QAbstractItemModel::modelReset, this, changed),
            connect(m_source, &QAbstractItemModel::layoutChanged, this, changed),
            // The source now shows another list: so do the matches (a queue playing them lets go).
            connect(m_source, &CollectionModel::sourceReset, this,
                    [this] {
                        m_hydrating = false;
                        if (m_matches != nullptr) {
                            m_matches->announceReset();
                        }
                    }),
            connect(m_source, &QObject::destroyed, this, [this] { Q_EMIT modelChanged(); }),
        };
    }
    if (m_matches != nullptr) {
        m_matches->announceReset();
    }
    refilter();
    Q_EMIT sourceChanged();
    Q_EMIT modelChanged();
}

void CollectionFilter::setText(const QString& text)
{
    if (text == m_text) {
        return;
    }
    const bool wasActive = active();
    m_text = text;
    QStringList words = CollectionFilter::words(text);
    const bool sameWords = words == m_words;
    m_words = std::move(words);
    Q_EMIT textChanged();
    if (sameWords) {
        return;  // only spacing changed
    }
    if (m_matches != nullptr) {
        m_matches->announceReset();  // other matches: a queue started from the last ones stops following them
    }
    refilter();
    if (active() != wasActive) {
        Q_EMIT modelChanged();
    }
}

void CollectionFilter::ensureModel()
{
    if (m_matches != nullptr && &m_matches->client() != &m_source->client()) {
        m_matches->deleteLater();
        m_matches = nullptr;
    }
    if (m_matches == nullptr) {
        m_matches = new CollectionModel(m_source->client(), this);  // NOLINT(cppcoreguidelines-owning-memory)
    }
}

void CollectionFilter::scheduleRefilter()
{
    if (!active() || m_queued) {
        return;
    }
    // A sync or a batch of fetched tracks changes the source many times in a row.
    m_queued = true;
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_queued = false;
            refilter();
        },
        Qt::QueuedConnection);
}

void CollectionFilter::refilter()
{
    if (!active()) {
        if (m_matches != nullptr && m_matches->count() > 0) {
            m_matches->clear();
        }
        return;
    }
    const bool fresh = m_matches == nullptr || &m_matches->client() != &m_source->client();
    ensureModel();
    if (!m_hydrating) {
        // Tracks known only by id (a long playlist's later ones) have no title to search yet.
        const bool stubs = std::ranges::any_of(m_source->entries(), [](const api::Entry& entry) {
            const auto* track = std::get_if<api::Track>(&entry.item);
            return track != nullptr && track->stub;
        });
        if (stubs) {
            m_hydrating = true;
            m_source->hydrateAll();
        }
    }
    std::vector<api::Entry> matching;
    for (const api::Entry& entry : m_source->entries()) {
        if (matches(entry, m_words)) {
            matching.push_back(entry);
        }
    }
    m_matches->applyEntries(std::move(matching));
    if (fresh) {
        Q_EMIT modelChanged();
    }
}

}  // namespace noctara::models
