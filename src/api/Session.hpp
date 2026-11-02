#pragma once

#include <QByteArray>
#include <QObject>

#include <mutex>

namespace noctara::api {

// The signed-in account's OAuth token, for the API client. Only ever held in memory: auth/ loads it from the
// secret store (slice f). Thread-safe; signals are emitted on the thread that caused them, so receivers on
// other threads get them queued.
class Session : public QObject {
    Q_OBJECT

public:
    explicit Session(QObject* parent = nullptr);

    [[nodiscard]] QByteArray token() const;
    [[nodiscard]] bool signedIn() const;
    void setToken(const QByteArray& token);
    void clear();

    // The API refused `token` (401). Emits rejected() once per token, unless the token was replaced meanwhile.
    void reportRejected(const QByteArray& token);

    // A short, stable digest of the token for keys (deduplication); never the token itself.
    [[nodiscard]] static QByteArray digest(const QByteArray& token);

Q_SIGNALS:
    void changed();
    // The token with this digest() no longer works: the UI asks the user to sign in again. Receivers on other
    // threads get it queued, so they compare the digest with the token current when it arrives.
    void rejected(const QByteArray& digest);

private:
    mutable std::mutex m_mutex;
    QByteArray m_token;
    QByteArray m_rejected;
};

}  // namespace noctara::api
