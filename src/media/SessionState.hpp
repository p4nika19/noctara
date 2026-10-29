#pragma once

#include "media/PlaybackQueue.hpp"

#include <QByteArray>
#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QTimer>

#include <vector>

namespace noctara::media {

// Player state that outlives a session (not a setting: it changes all the time).
struct SessionState {
    double volume = 0.7;  // slider position 0…1
    bool muted = false;
    bool shuffle = false;
    Repeat repeat = Repeat::Off;
    // The queue to restore: track ids in playing order, the current one and where it was.
    std::vector<qint64> queue;
    int current = -1;
    qint64 positionMs = 0;
    QString context;

    static constexpr std::size_t kMaxQueue = 500;

    [[nodiscard]] QByteArray toJson() const;
    // Unknown or damaged content yields the defaults.
    [[nodiscard]] static SessionState fromJson(const QByteArray& json);
    friend bool operator==(const SessionState&, const SessionState&) = default;
};

// Keeps <state>/session.json up to date: saves are debounced and written in order on a worker thread.
class SessionStore : public QObject {
    Q_OBJECT

public:
    // An empty path keeps the state in memory only.
    explicit SessionStore(QString path, QObject* parent = nullptr);
    ~SessionStore() override;
    SessionStore(const SessionStore&) = delete;
    SessionStore& operator=(const SessionStore&) = delete;
    SessionStore(SessionStore&&) = delete;
    SessionStore& operator=(SessionStore&&) = delete;

    [[nodiscard]] SessionState load() const;
    void save(const SessionState& state);
    void flush();

    static constexpr std::chrono::milliseconds kDebounce{1000};

private:
    QString m_path;
    SessionState m_pending;
    SessionState m_saved;
    QTimer m_timer;
    QThreadPool m_writer;
};

}  // namespace noctara::media
