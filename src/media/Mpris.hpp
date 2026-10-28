#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusObjectPath>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

namespace noctara::media {

class CoverFiles;
class PlaybackController;

struct MprisTrack {
    QString id;  // SoundCloud track id
    QString title;
    QString artist;
    QString album;  // the playlist or album it plays from
    qint64 durationMs = 0;
    QString artFile;  // local file; MPRIS clients must not fetch covers from the network themselves
    QString url;      // permalink
};

// MPRIS 2 metadata (a{sv}) for `track`; empty when nothing is loaded.
[[nodiscard]] QVariantMap mprisMetadata(const MprisTrack& track);
[[nodiscard]] QDBusObjectPath mprisTrackPath(const QString& id);

// Publishes the player on the session bus as org.mpris.MediaPlayer2.noctara (MPRIS 2), so media keys, the
// desktop's media controls and other apps can control playback. Follows the "media controls" setting live.
class MprisService : public QObject {
    Q_OBJECT

public:
    MprisService(PlaybackController& player, CoverFiles* covers, QObject* parent = nullptr);
    ~MprisService() override;
    MprisService(const MprisService&) = delete;
    MprisService& operator=(const MprisService&) = delete;
    MprisService(MprisService&&) = delete;
    MprisService& operator=(MprisService&&) = delete;

    void setEnabled(bool enabled);
    [[nodiscard]] bool registered() const noexcept { return !m_service.isEmpty(); }

    [[nodiscard]] PlaybackController& player() noexcept { return m_player; }
    [[nodiscard]] QVariantMap metadata() const;
    [[nodiscard]] QString playbackStatus() const;

Q_SIGNALS:
    // A client asked to bring the window to the front.
    void raiseRequested();

private:
    void propertiesChanged(const QString& interface, const QVariantMap& changed) const;
    void trackChanged();

    PlaybackController& m_player;
    CoverFiles* m_covers;
    QString m_service;  // registered bus name, empty while off
    QString m_artFile;
    QString m_artFor;  // the track the art file belongs to
};

// org.mpris.MediaPlayer2
class MprisRootAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ canQuit)
    Q_PROPERTY(bool CanRaise READ canRaise)
    Q_PROPERTY(bool HasTrackList READ hasTrackList)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ supportedUriSchemes)
    Q_PROPERTY(QStringList SupportedMimeTypes READ supportedMimeTypes)

public:
    explicit MprisRootAdaptor(MprisService* service);

    [[nodiscard]] static bool canQuit() { return true; }
    [[nodiscard]] static bool canRaise() { return true; }
    [[nodiscard]] static bool hasTrackList() { return false; }
    [[nodiscard]] static QString identity();
    [[nodiscard]] static QString desktopEntry();
    [[nodiscard]] static QStringList supportedUriSchemes() { return {}; }
    [[nodiscard]] static QStringList supportedMimeTypes() { return {}; }

public Q_SLOTS:
    void Raise();  // NOLINT(readability-identifier-naming): D-Bus method names
    void Quit();   // NOLINT(readability-identifier-naming)

private:
    MprisService* m_service;
};

// org.mpris.MediaPlayer2.Player
class MprisPlayerAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(QString LoopStatus READ loopStatus WRITE setLoopStatus)
    Q_PROPERTY(double Rate READ rate WRITE setRate)
    Q_PROPERTY(bool Shuffle READ shuffle WRITE setShuffle)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double MinimumRate READ rate)
    Q_PROPERTY(double MaximumRate READ rate)
    Q_PROPERTY(bool CanGoNext READ canGoNext)
    Q_PROPERTY(bool CanGoPrevious READ canGoPrevious)
    Q_PROPERTY(bool CanPlay READ canPlay)
    Q_PROPERTY(bool CanPause READ canPlay)
    Q_PROPERTY(bool CanSeek READ canPlay)
    Q_PROPERTY(bool CanControl READ canControl)

public:
    explicit MprisPlayerAdaptor(MprisService* service);

    [[nodiscard]] QString playbackStatus() const;
    [[nodiscard]] QString loopStatus() const;
    void setLoopStatus(const QString& status);
    [[nodiscard]] static double rate() { return 1.0; }
    static void setRate(double /*rate*/) {}
    [[nodiscard]] bool shuffle() const;
    void setShuffle(bool shuffle);
    [[nodiscard]] QVariantMap metadata() const;
    [[nodiscard]] double volume() const;
    void setVolume(double volume);
    [[nodiscard]] qlonglong position() const;
    [[nodiscard]] bool canGoNext() const;
    [[nodiscard]] bool canGoPrevious() const;
    [[nodiscard]] bool canPlay() const;
    [[nodiscard]] static bool canControl() { return true; }

public Q_SLOTS:
    // NOLINTBEGIN(readability-identifier-naming): D-Bus method names
    void Next();
    void Previous();
    void Pause();
    void PlayPause();
    void Stop();
    void Play();
    void Seek(qlonglong offset);
    void SetPosition(const QDBusObjectPath& trackId, qlonglong position);
    void OpenUri(const QString& uri);
    // NOLINTEND(readability-identifier-naming)

Q_SIGNALS:
    void Seeked(qlonglong position);  // NOLINT(readability-identifier-naming)

private:
    MprisService* m_service;
};

}  // namespace noctara::media
