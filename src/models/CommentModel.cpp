#include "models/CommentModel.hpp"

namespace noctara::models {

CommentModel::CommentModel(api::IApiClient& client, QObject* parent)
    : QAbstractListModel(parent)
    , m_client(client)
{
}

void CommentModel::clear()
{
    m_request.cancel();
    ++m_generation;
    m_next.clear();
    m_error = -1;
    m_loading = false;
    if (!m_comments.empty()) {
        beginResetModel();
        m_comments.clear();
        endResetModel();
        Q_EMIT countChanged();
    }
    Q_EMIT stateChanged();
}

void CommentModel::load(api::Id track, int pageSize)
{
    clear();
    m_first = api::endpoints::comments(track, pageSize);
    request(m_first);
}

void CommentModel::request(const api::Endpoint<api::Page<api::Comment>>& endpoint)
{
    m_loading = true;
    m_error = -1;
    Q_EMIT stateChanged();
    const int generation = m_generation;
    m_request = api::fetch(m_client, endpoint, this, [this, generation](Result<api::Page<api::Comment>> page) {
        if (generation != m_generation) {
            return;
        }
        m_loading = false;
        if (!page) {
            m_error = static_cast<int>(page.error().code);
            Q_EMIT stateChanged();
            return;
        }
        m_next = page->next;
        if (!page->items.empty()) {
            const int first = count();
            beginInsertRows({}, first, first + static_cast<int>(page->items.size()) - 1);
            m_comments.insert(m_comments.end(), std::make_move_iterator(page->items.begin()),
                              std::make_move_iterator(page->items.end()));
            endInsertRows();
            Q_EMIT countChanged();
        }
        Q_EMIT stateChanged();
    });
}

int CommentModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant CommentModel::data(const QModelIndex& index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid)) {
        return {};
    }
    const api::Comment& comment = m_comments.at(static_cast<std::size_t>(index.row()));
    switch (static_cast<Role>(role)) {
    case BodyRole: return comment.body;
    case UserRole: return comment.user.username;
    case UserIdRole: return QVariant::fromValue<qint64>(comment.user.id);
    case AvatarRole:
        return comment.user.avatarUrl.isEmpty() ? QString() : api::sizedArtwork(comment.user.avatarUrl, 67);
    case AtRole: return comment.createdAt;
    case PositionRole: return QVariant::fromValue<qint64>(comment.timestampMs);
    }
    return {};
}

QHash<int, QByteArray> CommentModel::roleNames() const
{
    return {{BodyRole, "body"},     {UserRole, "user"}, {UserIdRole, "userId"},
            {AvatarRole, "avatar"}, {AtRole, "at"},     {PositionRole, "position"}};
}

bool CommentModel::canFetchMore(const QModelIndex& parent) const
{
    return !parent.isValid() && !m_loading && !m_next.isEmpty();
}

void CommentModel::fetchMore(const QModelIndex& parent)
{
    if (canFetchMore(parent)) {
        request(api::endpoints::next(m_first, m_next));
    }
}

}  // namespace noctara::models
