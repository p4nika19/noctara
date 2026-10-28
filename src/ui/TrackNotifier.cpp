#include "ui/TrackNotifier.hpp"

#include "core/Paths.hpp"
#include "media/CoverFiles.hpp"
#include "media/PlaybackController.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QUrl>

namespace noctara::ui {

TrackNotifier::TrackNotifier(media::PlaybackController& player, settings::SettingsStore& settings,
                             media::CoverFiles* covers, std::function<bool()> windowActive, QObject* parent)
    : QObject(parent)
    , m_player(player)
    , m_settings(settings)
    , m_covers(covers)
    , m_windowActive(std::move(windowActive))
{
    connect(&m_player, &media::PlaybackController::trackChanged, this, &TrackNotifier::trackChanged);
}

void TrackNotifier::trackChanged()
{
    const QString id = m_player.trackId();
    if (id.isEmpty() || id == m_shownFor || !m_player.playing() ||
        !m_settings.boolValue(settings::keys::TrackNotifications) || (m_windowActive && m_windowActive())) {
        return;
    }
    m_shownFor = id;
    const QString title = m_player.title();
    const QString artist = m_player.artist();
    if (m_covers == nullptr) {
        send(title, artist, {});
        return;
    }
    m_covers->request(m_player.artworkUrl(), [this, id, title, artist](const QString& path) {
        if (id == m_player.trackId()) {
            send(title, artist, path);
        }
    });
}

void TrackNotifier::send(const QString& title, const QString& artist, const QString& cover)
{
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("/org/freedesktop/Notifications"),
        QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("Notify"));
    QVariantMap hints{{QStringLiteral("desktop-entry"), QString::fromLatin1(kAppId)},
                      {QStringLiteral("category"), QStringLiteral("x-noctara.track")},
                      {QStringLiteral("transient"), true}};
    if (!cover.isEmpty()) {
        hints.insert(QStringLiteral("image-path"), QUrl::fromLocalFile(cover).toString());
    }
    // The body may be shown as markup: SoundCloud text is escaped.
    call << QStringLiteral("Noctara") << m_replaces << QString() << title << artist.toHtmlEscaped() << QStringList()
         << hints << -1;
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): parented, deleted once the reply is in
    const auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* finished) {
        const QDBusPendingReply<uint> reply = *finished;
        if (reply.isValid()) {
            m_replaces = reply.value();
        }
        finished->deleteLater();
    });
}

}  // namespace noctara::ui
