#pragma once

#include "core/Error.hpp"
#include "net/Resolver.hpp"
#include "net/Transport.hpp"

#include <QDateTime>
#include <QMetaType>
#include <QString>

#include <cstdint>
#include <optional>

namespace noctara::net {

// One finished attempt, already redacted (no credentials, no query values), for the diagnostics panel.
struct RequestRecord {
    QDateTime finishedAt;
    QByteArray method;
    QString url;
    int status = 0;
    std::optional<ErrorCode> error;
    QString errorDetail;  // English, from the transport
    Timings timings;
    std::int64_t bytes = 0;
    QByteArray httpVersion;
    QString remoteAddress;
    QString route;  // "direct" or a proxy display URL
    ResolverSource dnsSource = ResolverSource::System;
    int attempt = 1;
    bool internal = false;  // the transport's own DoH queries
};

}  // namespace noctara::net

Q_DECLARE_METATYPE(noctara::net::RequestRecord)
