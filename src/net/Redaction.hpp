#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QUrl>

namespace noctara::net {

// URLs for logs, diagnostics and reports: user-info removed and every query value replaced, because signed
// media URLs and API calls carry tokens there (oauth_token, Signature, Policy, client_id …).
[[nodiscard]] QString redactUrl(const QUrl& url);

// Header values that carry credentials (Authorization, Proxy-Authorization, Cookie, Set-Cookie) are replaced.
[[nodiscard]] QByteArray redactHeader(QByteArrayView name, QByteArrayView value);

}  // namespace noctara::net
