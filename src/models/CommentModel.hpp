#pragma once

#include "api/ApiClient.hpp"
#include "api/Endpoints.hpp"
#include "api/Types.hpp"

#include <QAbstractListModel>

#include <vector>

namespace noctara::models {

// A track's comments for QML, newest first. Pages load lazily: views call fetchMore() near the end.
class CommentModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    // ErrorCode of the last failed load (as int), -1 while there is none.
    Q_PROPERTY(int errorCode READ errorCode NOTIFY stateChanged)

public:
    enum Role {
        BodyRole = Qt::UserRole + 1,
        UserRole,  // the commenter's name
        UserIdRole,
        AvatarRole,    // a 67 px variant
        AtRole,        // when it was written
        PositionRole,  // ms into the track, or -1
    };
    Q_ENUM(Role)

    explicit CommentModel(api::IApiClient& client, QObject* parent = nullptr);

    // Replaces the contents with the comments of `track` and loads the first page.
    void load(api::Id track, int pageSize);
    void clear();

    [[nodiscard]] int count() const noexcept { return static_cast<int>(m_comments.size()); }
    [[nodiscard]] bool loading() const noexcept { return m_loading; }
    [[nodiscard]] int errorCode() const noexcept { return m_error; }

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

Q_SIGNALS:
    void countChanged();
    void stateChanged();

private:
    void request(const api::Endpoint<api::Page<api::Comment>>& endpoint);

    api::IApiClient& m_client;
    api::Endpoint<api::Page<api::Comment>> m_first;
    std::vector<api::Comment> m_comments;
    QString m_next;
    net::RequestHandle m_request;
    bool m_loading = false;
    int m_error = -1;
    int m_generation = 0;  // answers to requests made before a load() or clear() are ignored
};

}  // namespace noctara::models
