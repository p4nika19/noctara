// noctara-mock-drm-helper: a stand-in for the protected-player helper. It speaks the helper protocol on stdin/stdout
// (electron/protocol.json) and simulates playback with timers. A separate QtCore-only binary: it starts in
// milliseconds, where anything linking the GUI stack pays for the GL driver's initialisation under ASan.
// Environment variables steer it:
//   NOCTARA_MOCK_DRM_WIDEVINE=0        report Widevine as unavailable
//   NOCTARA_MOCK_DRM_VERSION=<n>       report protocol version n
//   NOCTARA_MOCK_DRM_SILENT=1          never answer pings (a hung helper)
//   NOCTARA_MOCK_DRM_CRASH_ON_LOAD=1   abort on every "load"
//   NOCTARA_MOCK_DRM_CRASH_ONCE=<file> abort on "load" unless <file> exists (it is created first)
//   NOCTARA_MOCK_DRM_CHILD=1           start a child process in its group and log "child <pid>"
//   NOCTARA_MOCK_DRM_DURATION_MS=<n>   length of every track (default 3000)
//   NOCTARA_MOCK_DRM_CONFIGURE_ERROR=1 answer "configure" with an error outside a track (empty id)
// Every message it receives is echoed back as a "log" message "got <json>"; its arguments as "args <json>".
// A "load" whose URL contains "license-denied" fails with the "drm" error code.

#include "drm/Protocol.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSocketNotifier>
#include <QTimer>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace {

using namespace noctara;

class MockHelper : public QObject {
public:
    MockHelper()
        : m_stdin(STDIN_FILENO, QSocketNotifier::Read)
    {
        m_duration = qEnvironmentVariableIntegerValue("NOCTARA_MOCK_DRM_DURATION_MS").value_or(3000);
        connect(&m_stdin, &QSocketNotifier::activated, this, &MockHelper::readInput);
        connect(&m_tick, &QTimer::timeout, this, &MockHelper::tick);
        m_tick.setInterval(50);
        QJsonArray arguments;
        for (const QString& argument : QCoreApplication::arguments()) {
            arguments.append(argument);
        }
        log(QStringLiteral("args ") + QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact)));
        if (qEnvironmentVariableIntValue("NOCTARA_MOCK_DRM_CHILD") == 1) {
            const pid_t child = ::fork();
            if (child == 0) {
                for (;;) {
                    ::pause();
                }
            }
            log(QStringLiteral("child %1").arg(child));
        }
    }

private:
    static void send(const QJsonObject& message)
    {
        const QByteArray line = drm::encode(message);
        (void)std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), stdout);
        (void)std::fflush(stdout);
    }

    static void log(const QString& text)
    {
        send({{QStringLiteral("type"), QStringLiteral("log")},
              {QStringLiteral("level"), QStringLiteral("debug")},
              {QStringLiteral("message"), text}});
    }

    void state(const QString& value) const
    {
        send({{QStringLiteral("type"), QStringLiteral("state")},
              {QStringLiteral("id"), m_id},
              {QStringLiteral("state"), value}});
    }

    void position() const
    {
        send({{QStringLiteral("type"), QStringLiteral("position")},
              {QStringLiteral("id"), m_id},
              {QStringLiteral("ms"), static_cast<double>(currentMs())},
              {QStringLiteral("durationMs"), static_cast<double>(m_duration)}});
    }

    [[nodiscard]] qint64 currentMs() const
    {
        return std::min<qint64>(m_duration, m_base + (m_playing ? m_clock.elapsed() : 0));
    }

    void readInput()
    {
        std::array<char, 4096> chunk{};
        const ssize_t got = ::read(STDIN_FILENO, chunk.data(), chunk.size());
        if (got <= 0) {
            QCoreApplication::quit();  // Noctara is gone
            return;
        }
        m_buffer.append(chunk.data(), got);
        qsizetype newline = 0;
        while ((newline = m_buffer.indexOf('\n')) >= 0) {
            const QByteArray line = m_buffer.left(newline);
            m_buffer.remove(0, newline + 1);
            if (!m_silent) {
                log(QStringLiteral("got ") + QString::fromUtf8(line));
            }
            if (Result<QJsonObject> message = drm::decode(line, drm::Direction::ToHelper)) {
                handle(*message);
            }
        }
    }

    void handle(const QJsonObject& message)
    {
        const QString type = message.value(u"type").toString();
        if (type == u"hello") {
            send({{QStringLiteral("type"), QStringLiteral("ready")},
                  {QStringLiteral("version"),
                   qEnvironmentVariableIntegerValue("NOCTARA_MOCK_DRM_VERSION").value_or(drm::protocolVersion())},
                  {QStringLiteral("widevine"), qEnvironmentVariable("NOCTARA_MOCK_DRM_WIDEVINE") != u"0"},
                  {QStringLiteral("electron"), QStringLiteral("mock")}});
        } else if (type == u"configure") {
            if (qEnvironmentVariableIntValue("NOCTARA_MOCK_DRM_CONFIGURE_ERROR") == 1) {
                send({{QStringLiteral("type"), QStringLiteral("error")},
                      {QStringLiteral("id"), QString()},
                      {QStringLiteral("code"), QStringLiteral("network")},
                      {QStringLiteral("detail"), QStringLiteral("configure refused")}});
                return;
            }
            send({{QStringLiteral("type"), QStringLiteral("configured")},
                  {QStringLiteral("proxy"), message.value(u"proxy").toObject().value(u"mode").toString()},
                  {QStringLiteral("dns"), message.value(u"dns").toObject().value(u"mode").toString()}});
        } else if (type == u"ping") {
            if (!m_silent) {
                send(
                    {{QStringLiteral("type"), QStringLiteral("pong")}, {QStringLiteral("seq"), message.value(u"seq")}});
            }
        } else if (type == u"shutdown") {
            QCoreApplication::quit();
        } else if (type == u"load") {
            load(message);
        } else if (type == u"play" && !m_id.isEmpty()) {
            setPlaying(true);
        } else if (type == u"pause" && !m_id.isEmpty()) {
            setPlaying(false);
            state(QStringLiteral("paused"));
        } else if (type == u"seek" && !m_id.isEmpty()) {
            m_base = static_cast<qint64>(message.value(u"ms").toDouble());
            m_clock.restart();
            position();
        } else if (type == u"stop") {
            m_tick.stop();
            m_id.clear();
        }
    }

    void load(const QJsonObject& message)
    {
        if (qEnvironmentVariableIntValue("NOCTARA_MOCK_DRM_CRASH_ON_LOAD") == 1) {
            std::abort();
        }
        if (const QString marker = qEnvironmentVariable("NOCTARA_MOCK_DRM_CRASH_ONCE"); !marker.isEmpty()) {
            QFile file(marker);
            if (!file.exists()) {
                (void)file.open(QIODevice::WriteOnly);
                file.close();
                std::abort();
            }
        }
        m_id = message.value(u"id").toString();
        m_base = static_cast<qint64>(message.value(u"startMs").toDouble());
        m_playing = false;
        state(QStringLiteral("loading"));
        if (message.value(u"url").toString().contains(u"license-denied")) {
            send({{QStringLiteral("type"), QStringLiteral("error")},
                  {QStringLiteral("id"), m_id},
                  {QStringLiteral("code"), QStringLiteral("drm")},
                  {QStringLiteral("detail"), QStringLiteral("license refused")}});
            return;
        }
        const bool autoplay = message.value(u"autoplay").toBool();
        QTimer::singleShot(30, this, [this, id = m_id, autoplay] {
            if (id != m_id) {
                return;
            }
            position();
            if (autoplay) {
                setPlaying(true);
            } else {
                state(QStringLiteral("paused"));
            }
        });
    }

    void setPlaying(bool playing)
    {
        m_base = currentMs();
        m_clock.restart();
        m_playing = playing;
        if (playing) {
            m_tick.start();
            state(QStringLiteral("playing"));
        } else {
            m_tick.stop();
        }
    }

    void tick()
    {
        position();
        if (currentMs() >= m_duration) {
            m_tick.stop();
            m_playing = false;
            m_base = m_duration;
            state(QStringLiteral("ended"));
        }
    }

    QSocketNotifier m_stdin;
    QByteArray m_buffer;
    QTimer m_tick;
    QElapsedTimer m_clock;
    QString m_id;
    qint64 m_base = 0;
    qint64 m_duration = 3000;
    bool m_playing = false;
    bool m_silent = qEnvironmentVariableIntValue("NOCTARA_MOCK_DRM_SILENT") == 1;  // hung after the handshake
};

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    MockHelper helper;
    return QCoreApplication::exec();
}
