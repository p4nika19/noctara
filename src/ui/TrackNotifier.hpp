#pragma once

#include <QObject>
#include <QString>

#include <functional>

namespace noctara::settings {
class SettingsStore;
}
namespace noctara::media {
class CoverFiles;
class PlaybackController;
}  // namespace noctara::media

namespace noctara::ui {

// A desktop notification (org.freedesktop.Notifications) for each new track, when the "notify on track change"
// setting is on and the window is not in front. Each notification replaces the previous one; the cover is a
// local file, so the notification server never fetches it from the network.
class TrackNotifier : public QObject {
    Q_OBJECT

public:
    TrackNotifier(media::PlaybackController& player, settings::SettingsStore& settings, media::CoverFiles* covers,
                  std::function<bool()> windowActive, QObject* parent = nullptr);

private:
    void trackChanged();
    void send(const QString& title, const QString& artist, const QString& cover);

    media::PlaybackController& m_player;
    settings::SettingsStore& m_settings;
    media::CoverFiles* m_covers;
    std::function<bool()> m_windowActive;
    QString m_shownFor;  // track id of the last notification
    uint m_replaces = 0;
};

}  // namespace noctara::ui
