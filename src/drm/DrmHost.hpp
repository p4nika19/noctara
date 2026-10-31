#pragma once

#include "core/Error.hpp"
#include "drm/HelperNetwork.hpp"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>

class QProcess;

namespace noctara::drm {

// How to start the helper: the castlabs Electron binary and the arguments before Noctara's own switches (the
// helper app's folder; tests use their own binary with --mock-drm-helper).
struct HelperCommand {
    QString program;
    QStringList arguments;

    friend bool operator==(const HelperCommand&, const HelperCommand&) = default;
};

// The helper assembled by `xmake drm-helper` in `dir`: <dir>/electron/electron and <dir>/app/main.js.
[[nodiscard]] std::optional<HelperCommand> helperIn(const QString& dir);
// The configured folder when set, otherwise <application dir>/drm-helper, then <prefix>/lib/noctara/drm-helper.
[[nodiscard]] std::optional<HelperCommand> findHelper(const QString& configured, const QString& applicationDir);

// Owns the protected-player helper process (RAII). It starts lazily, on the first message; every start sends
// "hello" and the network configuration before anything else. A heartbeat detects a hung helper; a crash or hang
// while busy restarts it with backoff (and gives up after repeated crashes); it shuts down after an idle timeout.
// The helper runs in its own process group, which is killed on shutdown and destruction, and dies with Noctara
// (PR_SET_PDEATHSIG; stdin closing also ends it), so no process outlives the app. GUI thread.
class DrmHost : public QObject {
    Q_OBJECT

public:
    enum class Status : std::uint8_t {
        Stopped,   // not running (never started, idle shutdown, or stopped)
        Starting,  // running, handshake pending
        Ready,     // running and answered "ready" with Widevine available
        Failed,    // cannot run right now; see lastError()
    };

    struct Options {
        std::optional<HelperCommand> command;  // nullopt: not installed
        QString profileDir;                    // the helper's userData (<cache>/electron)
        bool gpu = false;
        std::chrono::milliseconds idleTimeout{std::chrono::minutes(5)};
        std::chrono::milliseconds heartbeat{std::chrono::seconds(5)};
        std::chrono::milliseconds heartbeatTimeout{std::chrono::seconds(15)};
        // Generous: the first start downloads the Widevine CDM (about 20 MB).
        std::chrono::milliseconds startTimeout{std::chrono::minutes(3)};
        std::chrono::milliseconds shutdownGrace{std::chrono::seconds(2)};
        std::chrono::milliseconds restartBackoff{500};  // doubled for every crash within kCrashWindow
    };

    explicit DrmHost(Options options, QObject* parent = nullptr);
    ~DrmHost() override;
    DrmHost(const DrmHost&) = delete;
    DrmHost& operator=(const DrmHost&) = delete;
    DrmHost(DrmHost&&) = delete;
    DrmHost& operator=(DrmHost&&) = delete;

    // Settings changes; they apply from the next start (the network applies live, see setNetwork()).
    void setCommand(std::optional<HelperCommand> command);
    void setGpu(bool gpu);
    void setIdleTimeout(std::chrono::milliseconds timeout);
    // Applies live through "configure"; a change of the startup switches restarts a running helper (stopped(true)
    // and ready() follow, so the backend resumes).
    void setNetwork(HelperNetwork network);
    [[nodiscard]] const HelperNetwork& network() const noexcept { return m_network; }

    // Sends a message, starting the helper first when it is not running. False when it cannot run now (failed()
    // has been emitted; see lastError()). Messages are validated; invalid ones are refused.
    bool send(const QJsonObject& message);
    // A protected track is loaded and playing or loading: no idle shutdown, and a crash restarts the helper.
    void setBusy(bool busy);
    // Starts the helper ahead of a protected track (the idle timeout applies).
    void warm();
    // Stops the helper now, gracefully (it is started again on the next message).
    void shutdown();

    [[nodiscard]] Status status() const noexcept { return m_status; }
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] const Error& lastError() const noexcept { return m_error; }
    [[nodiscard]] QString electronVersion() const { return m_electronVersion; }
    [[nodiscard]] qint64 processId() const noexcept { return m_pid; }

    static constexpr int kMaxCrashes = 4;  // within kCrashWindow, then it gives up until the next request
    static constexpr std::chrono::seconds kCrashWindow{60};

Q_SIGNALS:
    // Handshake done: the helper is ready for "load" (also after a restart).
    void ready();
    // A validated message from the helper other than the handshake and heartbeat: state, position, error,
    // configured, log.
    void received(const QJsonObject& message);
    // The process is gone. `unexpected`: it crashed, hung or was restarted; anything loaded is lost.
    void stopped(bool unexpected);
    // It cannot run (not installed, failed to start, no Widevine, protocol mismatch, kept crashing).
    void failed(const noctara::Error& error);
    void statusChanged();

private:
    bool start();
    void write(const QJsonObject& message);
    void onStdout();
    void onStderr();
    void onFinished();
    // The process has exited (or is being abandoned): forget it and report the stop.
    void reap(bool unexpected);
    // Ends a running process at once and waits for it (restarts and teardown).
    void terminateNow();
    void onMessage(const QJsonObject& message);
    void heartbeat();
    void scheduleRestart();
    void giveUp(const Error& error);
    void killGroup() const;
    void setStatus(Status status);
    void updateIdleTimer();
    [[nodiscard]] QStringList arguments() const;

    Options m_options;
    HelperNetwork m_network;
    QProcess* m_process = nullptr;  // child of the host; deleted later once it has exited
    qint64 m_pid = 0;
    Status m_status = Status::Stopped;
    Error m_error;
    QString m_electronVersion;
    QByteArray m_stdout;
    QByteArray m_stderr;
    bool m_busy = false;
    bool m_stopping = false;  // a stop Noctara asked for: the exit is expected
    QTimer m_idleTimer;
    QTimer m_heartbeatTimer;
    QTimer m_startTimer;
    QTimer m_restartTimer;
    QTimer m_killTimer;
    QElapsedTimer m_lastPong;
    double m_pingSeq = 0;
    std::deque<QElapsedTimer> m_crashes;
};

}  // namespace noctara::drm
