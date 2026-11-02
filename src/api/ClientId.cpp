#include "api/ClientId.hpp"

#include "core/FileIo.hpp"
#include "core/Log.hpp"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>
#include <chrono>

namespace noctara::api {

namespace clientid {

namespace {

bool idChar(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

QByteArrayView runAt(QByteArrayView text, qsizetype from)
{
    qsizetype end = from;
    while (end < text.size() && idChar(text[end])) {
        ++end;
    }
    return text.sliced(from, end - from);
}

qsizetype skipSpaces(QByteArrayView text, qsizetype at)
{
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) {
        ++at;
    }
    return at;
}

qsizetype skipQuote(QByteArrayView text, qsizetype at)
{
    return at < text.size() && (text[at] == '"' || text[at] == '\'') ? at + 1 : at;
}

std::optional<QString> accept(QByteArrayView run)
{
    const QString id = QString::fromLatin1(run);
    return plausible(id) ? std::optional(id) : std::nullopt;
}

std::optional<QString> fromHydration(QByteArrayView text)
{
    const qsizetype marker = text.indexOf(QByteArrayView(R"("hydratable":"apiClient")"));
    if (marker < 0) {
        return std::nullopt;
    }
    constexpr QByteArrayView kId(R"("id":")");
    const qsizetype at = text.indexOf(kId, marker);
    if (at < 0 || at - marker > 200) {
        return std::nullopt;
    }
    return accept(runAt(text, at + kId.size()));
}

}  // namespace

bool plausible(QStringView id) noexcept
{
    return id.size() >= 24 && id.size() <= 64 &&
           std::ranges::all_of(id, [](QChar c) { return c.unicode() < 128 && idChar(static_cast<char>(c.unicode())); });
}

std::optional<QString> find(QByteArrayView text)
{
    if (auto hydrated = fromHydration(text)) {
        return hydrated;
    }
    constexpr QByteArrayView kName("client_id");
    for (qsizetype at = text.indexOf(kName); at >= 0; at = text.indexOf(kName, at + 1)) {
        qsizetype i = skipSpaces(text, skipQuote(text, at + kName.size()));
        if (i >= text.size() || (text[i] != ':' && text[i] != '=' && text[i] != ',')) {
            continue;
        }
        i = skipQuote(text, skipSpaces(text, i + 1));
        if (auto id = accept(runAt(text, i))) {
            return id;
        }
    }
    return std::nullopt;
}

std::vector<QUrl> scripts(QByteArrayView html, const QUrl& page)
{
    static const QRegularExpression tag(QStringLiteral(R"(<script\b[^>]*\bsrc\s*=\s*["']([^"']+)["'])"),
                                        QRegularExpression::CaseInsensitiveOption);
    std::vector<QUrl> result;
    const QString text = QString::fromUtf8(html);
    for (auto it = tag.globalMatch(text); it.hasNext();) {
        const QUrl url = page.resolved(QUrl(it.next().captured(1)));
        if (url.scheme() == u"https" && url.host().endsWith(u".sndcdn.com") && !std::ranges::contains(result, url)) {
            result.push_back(url);
        }
    }
    return result;
}

}  // namespace clientid

namespace {

const QUrl& homePage()
{
    static const QUrl url(QStringLiteral("https://soundcloud.com/"));
    return url;
}

net::Request plainGet(const QUrl& url)
{
    net::Request request;
    request.url = url;
    request.cache = net::CacheMode::Bypass;  // the id is looked up rarely, and the page must be current
    request.timeout = std::chrono::seconds(30);
    return request;
}

}  // namespace

ClientIdProvider::ClientIdProvider(net::INetworkTransport& transport, QString storeFile, QObject* parent)
    : QObject(parent)
    , m_transport(transport)
    , m_storeFile(std::move(storeFile))
{
    if (m_storeFile.isEmpty()) {
        return;
    }
    if (Result<QByteArray> data = readWholeFile(m_storeFile)) {
        const QString id = QJsonDocument::fromJson(*data).object().value(u"client_id").toString();
        if (clientid::plausible(id)) {
            m_known = id;
            m_knownSource = Source::Stored;
        }
    }
}

void ClientIdProvider::setOverride(const QString& id)
{
    if (id == m_override) {
        return;
    }
    m_override = id;
    m_rejected.remove(id);
    Q_EMIT changed();
}

QString ClientIdProvider::current() const
{
    if (!m_override.isEmpty()) {
        return m_rejected.contains(m_override) ? QString() : m_override;
    }
    return m_known;
}

ClientIdProvider::Source ClientIdProvider::source() const noexcept
{
    return m_override.isEmpty() ? m_knownSource : Source::Override;
}

void ClientIdProvider::acquire(std::function<void(Result<QString>)> done)
{
    if (const QString id = current(); !id.isEmpty()) {
        done(id);
        return;
    }
    if (!m_override.isEmpty()) {
        done(fail(ErrorCode::ClientIdUnavailable, "the configured client_id was rejected by the API"));
        return;
    }
    if (m_lastFailure && std::chrono::steady_clock::now() - m_failedAt < kRetryAfterFailure) {
        done(std::unexpected(*m_lastFailure));
        return;
    }
    m_waiting.push_back(std::move(done));
    if (!m_discovering) {
        discover();
    }
}

void ClientIdProvider::invalidate(const QString& id)
{
    if (id.isEmpty() || m_rejected.contains(id)) {
        return;
    }
    qCInfo(lcApi) << "SoundCloud rejected the client_id; looking for a new one";
    m_rejected.insert(id);
    if (id == m_known) {
        m_known.clear();
        m_knownSource = Source::None;
    }
    Q_EMIT changed();
}

void ClientIdProvider::discover()
{
    m_discovering = true;
    m_transport.send(plainGet(homePage()), this, net::whenFinished([this](Result<net::Response> response) {
                         if (!response) {
                             finish(std::unexpected(response.error()));
                             return;
                         }
                         if (!response->successful()) {
                             finish(fail(ErrorCode::ClientIdUnavailable,
                                         "soundcloud.com answered " + std::to_string(response->status)));
                             return;
                         }
                         if (auto id = clientid::find(response->body); id && !m_rejected.contains(*id)) {
                             finish(*id);
                             return;
                         }
                         m_scripts = clientid::scripts(response->body, response->url);
                         // The id sits in one of the last bundles; search from the end, a bounded number.
                         std::ranges::reverse(m_scripts);
                         if (m_scripts.size() > kMaxScripts) {
                             m_scripts.resize(kMaxScripts);
                         }
                         fetchScript();
                     }));
}

void ClientIdProvider::fetchScript()
{
    if (m_scripts.empty()) {
        finish(fail(ErrorCode::ClientIdUnavailable, "no client_id in soundcloud.com or its scripts"));
        return;
    }
    const QUrl url = m_scripts.front();
    m_scripts.erase(m_scripts.begin());
    m_transport.send(plainGet(url), this, net::whenFinished([this](Result<net::Response> response) {
                         if (!response) {
                             finish(std::unexpected(response.error()));
                             return;
                         }
                         if (response->successful()) {
                             if (auto id = clientid::find(response->body); id && !m_rejected.contains(*id)) {
                                 finish(*id);
                                 return;
                             }
                         }
                         fetchScript();
                     }));
}

void ClientIdProvider::finish(Result<QString> result)
{
    m_discovering = false;
    m_scripts.clear();
    if (result) {
        qCInfo(lcApi) << "found SoundCloud's client_id";
        m_known = *result;
        m_knownSource = Source::Discovered;
        m_lastFailure.reset();
        store(m_known);
        Q_EMIT changed();
    } else {
        qCWarning(lcApi).noquote() << "client_id discovery failed:"
                                   << QString::fromStdString(result.error().describe());
        // Network failures are not the site's fault: only throttle when the site was reachable.
        if (result.error().code == ErrorCode::ClientIdUnavailable) {
            m_lastFailure = result.error();
            m_failedAt = std::chrono::steady_clock::now();
        }
    }
    for (const auto& done : std::exchange(m_waiting, {})) {
        done(result);
    }
}

void ClientIdProvider::store(const QString& id)
{
    if (m_storeFile.isEmpty()) {
        return;
    }
    const QJsonObject object{{QStringLiteral("client_id"), id},
                             {QStringLiteral("found_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    if (Result<void> written = writeFileAtomic(m_storeFile, QJsonDocument(object).toJson(QJsonDocument::Compact), 0600,
                                               Durability::Atomic);
        !written) {
        qCWarning(lcApi).noquote() << "could not store the client_id:"
                                   << QString::fromStdString(written.error().describe());
    }
}

}  // namespace noctara::api
