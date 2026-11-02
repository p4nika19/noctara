#include "api/Session.hpp"

#include <QCryptographicHash>

namespace noctara::api {

Session::Session(QObject* parent)
    : QObject(parent)
{
}

QByteArray Session::token() const
{
    const std::scoped_lock lock(m_mutex);
    return m_token;
}

bool Session::signedIn() const
{
    const std::scoped_lock lock(m_mutex);
    return !m_token.isEmpty();
}

void Session::setToken(const QByteArray& token)
{
    {
        const std::scoped_lock lock(m_mutex);
        if (token == m_token) {
            return;
        }
        m_token = token;
        m_rejected.clear();
    }
    Q_EMIT changed();
}

void Session::clear()
{
    setToken({});
}

void Session::reportRejected(const QByteArray& token)
{
    {
        const std::scoped_lock lock(m_mutex);
        if (token.isEmpty() || token != m_token || token == m_rejected) {
            return;
        }
        m_rejected = token;
    }
    Q_EMIT rejected(digest(token));
}

QByteArray Session::digest(const QByteArray& token)
{
    if (token.isEmpty()) {
        return {};
    }
    return QCryptographicHash::hash(token, QCryptographicHash::Sha256).toHex().left(16);
}

}  // namespace noctara::api
