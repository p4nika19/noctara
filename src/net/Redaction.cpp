#include "net/Redaction.hpp"

#include "core/Log.hpp"

#include <QUrlQuery>

#include <array>

namespace noctara::net {

QString redactUrl(const QUrl& url)
{
    QUrl copy = url;
    if (!copy.userInfo().isEmpty()) {
        copy.setUserInfo(log::redacted());
    }
    copy.setFragment(QString());
    if (copy.hasQuery()) {
        const QUrlQuery query(copy);
        QUrlQuery masked;
        for (const auto& [key, value] : query.queryItems(QUrl::FullyDecoded)) {
            masked.addQueryItem(key, value.isEmpty() ? QString() : log::redacted());
        }
        copy.setQuery(masked);
    }
    return copy.toDisplayString(QUrl::DecodeReserved);
}

QByteArray redactHeader(QByteArrayView name, QByteArrayView value)
{
    constexpr std::array secret{"authorization", "proxy-authorization", "cookie", "set-cookie"};
    const QByteArray lower = name.toByteArray().toLower();
    if (std::ranges::any_of(secret, [&](const char* header) { return lower == header; })) {
        return log::redacted().toLatin1();
    }
    return value.toByteArray();
}

}  // namespace noctara::net
