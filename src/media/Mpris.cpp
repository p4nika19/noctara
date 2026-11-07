#include "media/Mpris.hpp"

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "media/CoverFiles.hpp"
#include "media/PlaybackController.hpp"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QUrl>

#include <unistd.h>

namespace noctara::media {

namespace {

constexpr QLatin1StringView kObjectPath("/org/mpris/MediaPlayer2");
constexpr QLatin1StringView kPlayerInterface("org.mpris.MediaPlayer2.Player");

constexpr qlonglong kUsPerMs = 1000;

}  // namespace

QDBusObjectPath mprisTrackPath(const QString& id)
{
    // Object paths allow [A-Za-z0-9_] only; SoundCloud ids are numeric.
    QString safe = id;
    for (QChar& c : safe) {
        if (!c.isLetterOrNumber() || c.unicode() > 127) {
            c = u'_';
        }
    }
    return QDBusObjectPath(QStringLiteral("/org/noctara/track/t") + safe);
}

QVariantMap mprisMetadata(const MprisTrack& track)
{
    if (track.id.isEmpty()) {
        return {{QStringLiteral("mpris:trackid"),
                 QVariant::fromValue(QDBusObjectPath(QStringLiteral("/org/mpris/MediaPlayer2/TrackList/NoTrack")))}};
    }
    QVariantMap map{
        {QStringLiteral("mpris:trackid"), QVariant::fromValue(mprisTrackPath(track.id))},
        {QStringLiteral("xesam:title"), track.title},
        {QStringLiteral("xesam:artist"), QStringList{track.artist}},
    };
    if (track.durationMs > 0) {
        map.insert(QStringLiteral("mpris:length"), QVariant::fromValue<qlonglong>(track.durationMs * kUsPerMs));
    }
    if (!track.album.isEmpty()) {
        map.insert(QStringLiteral("xesam:album"), track.album);
    }
    if (!track.artFile.isEmpty()) {
        map.insert(QStringLiteral("mpris:artUrl"), QUrl::fromLocalFile(track.artFile).toString());
    }
    if (!track.url.isEmpty()) {
        map.insert(QStringLiteral("xesam:url"), track.url);
    }
    return map;
}

// --- Service -------------------------------------------------------------------------------------

MprisService::MprisService(PlaybackController& player, CoverFiles* covers, QObject* parent)
    : QObject(parent)
    , m_player(player)
    , m_covers(covers)
{
    new MprisRootAdaptor(this);                    // NOLINT(cppcoreguidelines-owning-memory): owned by this QObject
    auto* adaptor = new MprisPlayerAdaptor(this);  // NOLINT(cppcoreguidelines-owning-memory): owned by this QObject
    connect(&m_player, &PlaybackController::trackChanged, this, &MprisService::trackChanged);
    connect(&m_player, &PlaybackController::durationChanged, this,
            [this] { propertiesChanged(kPlayerInterface, {{QStringLiteral("Metadata"), metadata()}}); });
    connect(&m_player, &PlaybackController::stateChanged, this,
            [this] { propertiesChanged(kPlayerInterface, {{QStringLiteral("PlaybackStatus"), playbackStatus()}}); });
    connect(&m_player, &PlaybackController::volumeChanged, this, [this] {
        propertiesChanged(kPlayerInterface, {{QStringLiteral("Volume"), m_player.muted() ? 0.0 : m_player.volume()}});
    });
    connect(&m_player, &PlaybackController::modesChanged, this, [this, adaptor] {
        propertiesChanged(kPlayerInterface, {{QStringLiteral("LoopStatus"), adaptor->loopStatus()},
                                             {QStringLiteral("Shuffle"), m_player.shuffle()}});
    });
    connect(&m_player, &PlaybackController::queueChanged, this, [this] {
        propertiesChanged(kPlayerInterface, {{QStringLiteral("CanGoNext"), m_player.canGoNext()},
                                             {QStringLiteral("CanGoPrevious"), m_player.canGoPrevious()}});
    });
    connect(&m_player, &PlaybackController::seeked, adaptor,
            [adaptor](qint64 ms) { Q_EMIT adaptor->Seeked(ms * kUsPerMs); });
}

MprisService::~MprisService()
{
    setEnabled(false);
}

void MprisService::setEnabled(bool enabled)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (enabled == registered() || !bus.isConnected()) {
        return;
    }
    if (!enabled) {
        bus.unregisterObject(kObjectPath);
        bus.unregisterService(m_service);
        m_service.clear();
        return;
    }
    if (!bus.registerObject(kObjectPath, this, QDBusConnection::ExportAdaptors)) {
        qCInfo(lcMedia) << "MPRIS: could not register the player object";
        return;
    }
    // A second instance takes a name of its own, as the specification suggests.
    QString name = QStringLiteral("org.mpris.MediaPlayer2.") + QString::fromLatin1(kAppId);
    if (!bus.registerService(name)) {
        name += QStringLiteral(".instance") + QString::number(::getpid());
        if (!bus.registerService(name)) {
            qCInfo(lcMedia) << "MPRIS: could not register a bus name";
            bus.unregisterObject(kObjectPath);
            return;
        }
    }
    m_service = std::move(name);
}

void MprisService::trackChanged()
{
    const QString url = m_player.artworkUrl();
    if (m_covers != nullptr && url != m_artFor) {
        m_artFor = url;
        m_artFile.clear();
        m_covers->request(url, [this, url](const QString& path) {
            if (url == m_artFor && path != m_artFile) {
                m_artFile = path;
                propertiesChanged(kPlayerInterface, {{QStringLiteral("Metadata"), metadata()}});
            }
        });
    }
    propertiesChanged(kPlayerInterface, trackProperties());
}

QVariantMap MprisService::trackProperties() const
{
    // CanPause and CanSeek follow CanPlay: clients caching them must hear about every change.
    return {{QStringLiteral("Metadata"), metadata()},
            {QStringLiteral("CanGoNext"), m_player.canGoNext()},
            {QStringLiteral("CanGoPrevious"), m_player.canGoPrevious()},
            {QStringLiteral("CanPlay"), m_player.hasTrack()},
            {QStringLiteral("CanPause"), m_player.hasTrack()},
            {QStringLiteral("CanSeek"), m_player.hasTrack()}};
}

QVariantMap MprisService::metadata() const
{
    if (!m_player.hasTrack()) {
        return mprisMetadata({});
    }
    return mprisMetadata({.id = m_player.trackId(),
                          .title = m_player.title(),
                          .artist = m_player.artist(),
                          .album = m_player.context(),
                          .durationMs = m_player.duration(),
                          .artFile = m_artFor == m_player.artworkUrl() ? m_artFile : QString(),
                          .url = m_player.permalink()});
}

QString MprisService::playbackStatus() const
{
    if (!m_player.hasTrack()) {
        return QStringLiteral("Stopped");
    }
    return m_player.playing() ? QStringLiteral("Playing") : QStringLiteral("Paused");
}

void MprisService::propertiesChanged(const QString& interface, const QVariantMap& changed) const
{
    if (!registered()) {
        return;
    }
    QDBusMessage signal = QDBusMessage::createSignal(kObjectPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                                                     QStringLiteral("PropertiesChanged"));
    signal << interface << changed << QStringList();
    (void)QDBusConnection::sessionBus().send(signal);
}

// --- org.mpris.MediaPlayer2 ----------------------------------------------------------------------

MprisRootAdaptor::MprisRootAdaptor(MprisService* service)
    : QDBusAbstractAdaptor(service)
    , m_service(service)
{
}

QString MprisRootAdaptor::identity()
{
    return QStringLiteral("Noctara");
}

QString MprisRootAdaptor::desktopEntry()
{
    return QString::fromLatin1(kAppId);
}

void MprisRootAdaptor::Raise()
{
    Q_EMIT m_service->raiseRequested();
}

void MprisRootAdaptor::Quit()
{
    QCoreApplication::quit();
}

// --- org.mpris.MediaPlayer2.Player ---------------------------------------------------------------

MprisPlayerAdaptor::MprisPlayerAdaptor(MprisService* service)
    : QDBusAbstractAdaptor(service)
    , m_service(service)
{
}

QString MprisPlayerAdaptor::playbackStatus() const
{
    return m_service->playbackStatus();
}

QString MprisPlayerAdaptor::loopStatus() const
{
    const QString repeat = m_service->player().repeat();
    if (repeat == u"one") {
        return QStringLiteral("Track");
    }
    return repeat == u"all" ? QStringLiteral("Playlist") : QStringLiteral("None");
}

void MprisPlayerAdaptor::setLoopStatus(const QString& status)
{
    if (status == u"Track") {
        m_service->player().setRepeat(QStringLiteral("one"));
    } else if (status == u"Playlist") {
        m_service->player().setRepeat(QStringLiteral("all"));
    } else if (status == u"None") {
        m_service->player().setRepeat(QStringLiteral("off"));
    }
}

bool MprisPlayerAdaptor::shuffle() const
{
    return m_service->player().shuffle();
}

void MprisPlayerAdaptor::setShuffle(bool shuffle)
{
    m_service->player().setShuffle(shuffle);
}

QVariantMap MprisPlayerAdaptor::metadata() const
{
    return m_service->metadata();
}

double MprisPlayerAdaptor::volume() const
{
    const PlaybackController& player = m_service->player();
    return player.muted() ? 0.0 : player.volume();
}

void MprisPlayerAdaptor::setVolume(double volume)
{
    m_service->player().setVolume(volume);
}

qlonglong MprisPlayerAdaptor::position() const
{
    return m_service->player().position() * kUsPerMs;
}

bool MprisPlayerAdaptor::canGoNext() const
{
    return m_service->player().canGoNext();
}

bool MprisPlayerAdaptor::canGoPrevious() const
{
    return m_service->player().canGoPrevious();
}

bool MprisPlayerAdaptor::canPlay() const
{
    return m_service->player().hasTrack();
}

void MprisPlayerAdaptor::Next()
{
    m_service->player().next();
}

void MprisPlayerAdaptor::Previous()
{
    m_service->player().previous();
}

void MprisPlayerAdaptor::Pause()
{
    m_service->player().pause();
}

void MprisPlayerAdaptor::PlayPause()
{
    m_service->player().playPause();
}

void MprisPlayerAdaptor::Stop()
{
    m_service->player().pause();
    m_service->player().seek(0);
}

void MprisPlayerAdaptor::Play()
{
    m_service->player().play();
}

void MprisPlayerAdaptor::Seek(qlonglong offset)
{
    PlaybackController& player = m_service->player();
    const qint64 target = player.position() + (offset / kUsPerMs);
    if (player.duration() > 0 && target >= player.duration()) {
        player.next();  // as the specification asks
        return;
    }
    player.seek(std::max<qint64>(target, 0));
}

void MprisPlayerAdaptor::SetPosition(const QDBusObjectPath& trackId, qlonglong position)
{
    PlaybackController& player = m_service->player();
    if (trackId != mprisTrackPath(player.trackId()) || position < 0) {
        return;  // a stale request for a track that is no longer current
    }
    player.seek(position / kUsPerMs);
}

void MprisPlayerAdaptor::OpenUri(const QString& /*uri*/)
{
    // SupportedUriSchemes is empty: nothing to open.
}

}  // namespace noctara::media
