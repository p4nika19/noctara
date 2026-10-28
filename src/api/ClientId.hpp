#pragma once

#include "core/Error.hpp"
#include "net/Transport.hpp"

#include <QByteArrayView>
#include <QDateTime>
#include <QObject>
#include <QSet>
#include <QString>
#include <QUrl>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

// api-v2 wants the client_id of SoundCloud's own web player. It is public (embedded in the site's JavaScript)
// but rotates, so it is discovered at runtime. Everything that depends on how soundcloud.com is built lives in
// this file, so a change of the site needs a fix here only. See docs/api.md.
namespace noctara::api {

namespace clientid {

// A client_id in a page or script: the hydration data ({"hydratable":"apiClient","data":{"id":"…"}}) or an
// assignment such as client_id:"…", client_id="…" or client_id=… in a URL.
[[nodiscard]] std::optional<QString> find(QByteArrayView text);

// The page's scripts served from SoundCloud's asset CDN (*.sndcdn.com), in page order.
[[nodiscard]] std::vector<QUrl> scripts(QByteArrayView html, const QUrl& page);

[[nodiscard]] bool plausible(QStringView id) noexcept;

}  // namespace clientid

// Provides the client_id: the user's override (Advanced settings), else the last one found (kept in a small
// state file), else discovers one from soundcloud.com. Discovery is single-flight and throttled after a failure.
// Lives on the API thread; all members must be called there.
class ClientIdProvider : public QObject {
    Q_OBJECT

public:
    enum class Source : std::uint8_t { None, Override, Stored, Discovered };

    // `storeFile` keeps the last discovered id between runs (empty: memory only).
    ClientIdProvider(net::INetworkTransport& transport, QString storeFile, QObject* parent = nullptr);

    void setOverride(const QString& id);
    [[nodiscard]] QString current() const;
    [[nodiscard]] Source source() const noexcept;

    // Calls `done` with a usable id: at once when one is known, else after discovery.
    void acquire(std::function<void(Result<QString>)> done);
    // The API rejected `id` (401 on an anonymous request): it is never used again; the next acquire()
    // discovers a new one. An overridden id is reported as unusable instead.
    void invalidate(const QString& id);

    static constexpr int kMaxScripts = 8;
    static constexpr std::chrono::seconds kRetryAfterFailure{30};

Q_SIGNALS:
    void changed();

private:
    void discover();
    void fetchScript();
    void finish(Result<QString> result);
    void store(const QString& id);

    net::INetworkTransport& m_transport;
    QString m_storeFile;
    QString m_override;
    QString m_known;
    Source m_knownSource = Source::None;
    QSet<QString> m_rejected;
    std::vector<std::function<void(Result<QString>)>> m_waiting;
    std::vector<QUrl> m_scripts;  // still to search, last first
    bool m_discovering = false;
    std::optional<Error> m_lastFailure;
    QDateTime m_failedAt;
};

}  // namespace noctara::api
