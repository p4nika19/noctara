#include "drm/DrmHost.hpp"

#include "core/Log.hpp"
#include "core/Trace.hpp"
#include "drm/Protocol.hpp"

#include <QDir>
#include <QFileInfo>
#include <QProcess>

#include <csignal>
#include <sys/prctl.h>
#include <unistd.h>

namespace noctara::drm {

namespace {

constexpr qsizetype kMaxLine = qsizetype{1024} * 1024;  // a longer line from the helper is a bug: dropped

QJsonObject typed(const char* type)
{
    return QJsonObject{{QStringLiteral("type"), QString::fromLatin1(type)}};
}

}  // namespace

std::optional<HelperCommand> helperIn(const QString& dir)
{
    if (dir.isEmpty()) {
        return std::nullopt;
    }
    const QString electron = dir + QStringLiteral("/electron/electron");
    const QString app = dir + QStringLiteral("/app");
    if (!QFileInfo(electron).isExecutable() || !QFileInfo::exists(app + QStringLiteral("/main.js"))) {
        return std::nullopt;
    }
    return HelperCommand{.program = QFileInfo(electron).absoluteFilePath(),
                         .arguments = {QFileInfo(app).absoluteFilePath()}};
}

std::optional<HelperCommand> findHelper(const QString& configured, const QString& applicationDir)
{
    if (!configured.isEmpty()) {
        return helperIn(configured);
    }
    const QStringList candidates{applicationDir + QStringLiteral("/drm-helper"),
                                 applicationDir + QStringLiteral("/../lib/noctara/drm-helper")};
    for (const QString& dir : candidates) {
        if (auto command = helperIn(QDir::cleanPath(dir))) {
            return command;
        }
    }
    return std::nullopt;
}

DrmHost::DrmHost(Options options, QObject* parent)
    : QObject(parent)
    , m_options(std::move(options))
{
    m_idleTimer.setSingleShot(true);
    connect(&m_idleTimer, &QTimer::timeout, this, [this] {
        qCInfo(lcDrm) << "closing the protected player after" << m_options.idleTimeout.count() / 1000 << "s idle";
        shutdown();
    });
    connect(&m_heartbeatTimer, &QTimer::timeout, this, &DrmHost::heartbeat);
    m_startTimer.setSingleShot(true);
    connect(&m_startTimer, &QTimer::timeout, this, [this] {
        qCWarning(lcDrm) << "the protected player did not get ready in time";
        killGroup();  // the exit is reported as a crash
    });
    m_restartTimer.setSingleShot(true);
    connect(&m_restartTimer, &QTimer::timeout, this, [this] {
        if (m_busy && !running()) {
            (void)start();
        }
    });
    m_killTimer.setSingleShot(true);
    connect(&m_killTimer, &QTimer::timeout, this, &DrmHost::killGroup);
}

DrmHost::~DrmHost()
{
    if (m_process == nullptr) {
        return;
    }
    m_process->disconnect(this);
    if (m_process->state() != QProcess::NotRunning) {
        m_process->write(encode(typed("shutdown")));
        m_process->closeWriteChannel();
        if (!m_process->waitForFinished(static_cast<int>(m_options.shutdownGrace.count()))) {
            killGroup();
            (void)m_process->waitForFinished(1000);
        }
    }
    killGroup();  // whatever the helper left behind in its group
}

void DrmHost::setCommand(std::optional<HelperCommand> command)
{
    m_options.command = std::move(command);
}

void DrmHost::setGpu(bool gpu)
{
    m_options.gpu = gpu;
}

void DrmHost::setIdleTimeout(std::chrono::milliseconds timeout)
{
    m_options.idleTimeout = timeout;
    m_idleTimer.stop();
    updateIdleTimer();
}

void DrmHost::setNetwork(HelperNetwork network)
{
    const bool switchesChanged = network.switches != m_network.switches;
    m_network = std::move(network);
    if (!running()) {
        return;
    }
    if (m_network.blocked) {
        giveUp(*m_network.blocked);
        return;
    }
    if (!switchesChanged) {
        write(m_network.configure);
        return;
    }
    // Proxy servers and host mappings are startup switches: start again with the new ones.
    qCInfo(lcDrm) << "restarting the protected player for new network settings";
    terminateNow();
    reap(true);
    if (m_busy) {
        (void)start();
    }
}

bool DrmHost::running() const noexcept
{
    return m_process != nullptr && m_process->state() != QProcess::NotRunning && !m_stopping;
}

bool DrmHost::send(const QJsonObject& message)
{
    if (const QString problem = validate(Direction::ToHelper, message); !problem.isEmpty()) {
        qCWarning(lcDrm).noquote() << "refusing to send an invalid message:" << problem;
        return false;
    }
    if (!running() && !start()) {
        return false;
    }
    write(message);
    return true;
}

void DrmHost::setBusy(bool busy)
{
    m_busy = busy;
    updateIdleTimer();
}

void DrmHost::warm()
{
    if (!running()) {
        (void)start();
    }
}

void DrmHost::shutdown()
{
    m_restartTimer.stop();
    if (!running()) {
        return;
    }
    write(typed("shutdown"));
    m_process->closeWriteChannel();  // a helper that misses the message quits on end of input
    m_stopping = true;
    m_idleTimer.stop();
    m_killTimer.start(m_options.shutdownGrace);
}

bool DrmHost::start()
{
    m_restartTimer.stop();
    if (m_process != nullptr) {
        // Still on its way out (a graceful stop): do not wait for it.
        terminateNow();
        reap(false);
    }
    if (m_network.blocked) {
        giveUp(*m_network.blocked);
        return false;
    }
    if (!m_options.command) {
        giveUp(Error{ErrorCode::ProtectedPlayerMissing, "no helper installed"});
        return false;
    }
    if (!m_options.profileDir.isEmpty()) {
        (void)QDir().mkpath(m_options.profileDir);
    }
    m_process = new QProcess(this);  // NOLINT(cppcoreguidelines-owning-memory): child of the host
    m_process->setProgram(m_options.command->program);
    m_process->setArguments(arguments());
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const char* name : {"ELECTRON_RUN_AS_NODE", "ELECTRON_ENABLE_LOGGING", "NODE_OPTIONS"}) {
        environment.remove(QString::fromLatin1(name));  // each would change what the helper is
    }
    m_process->setProcessEnvironment(environment);
    // Its own process group (killed as a whole), no inherited descriptors or signal dispositions, and death
    // together with Noctara even if Noctara is killed.
    m_process->setUnixProcessParameters(QProcess::UnixProcessParameters{
        .flags = QProcess::UnixProcessFlag::CreateNewSession | QProcess::UnixProcessFlag::ResetSignalHandlers |
                 QProcess::UnixProcessFlag::CloseFileDescriptors,
        .lowestFileDescriptorToClose = STDERR_FILENO + 1});
    m_process->setChildProcessModifier([] { (void)::prctl(PR_SET_PDEATHSIG, SIGKILL); });
    connect(m_process, &QProcess::readyReadStandardOutput, this, &DrmHost::onStdout);
    connect(m_process, &QProcess::readyReadStandardError, this, &DrmHost::onStderr);
    connect(m_process, &QProcess::finished, this, &DrmHost::onFinished);
    m_process->start();
    if (!m_process->waitForStarted(5000)) {
        const QString why = m_process->errorString();
        m_process->disconnect(this);
        m_process->deleteLater();
        m_process = nullptr;
        giveUp(Error{ErrorCode::ProtectedPlayerFailed, "could not start: " + why.toStdString()});
        return false;
    }
    m_pid = m_process->processId();
    m_stopping = false;
    m_stdout.clear();
    m_stderr.clear();
    NOCTARA_TRACE_INSTANT("drm: helper started");
    qCInfo(lcDrm) << "protected player started, pid" << m_pid;
    setStatus(Status::Starting);
    m_lastPong.start();
    m_heartbeatTimer.start(m_options.heartbeat);
    m_startTimer.start(m_options.startTimeout);
    write(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("hello")}, {QStringLiteral("version"), protocolVersion()}});
    write(m_network.configure);
    updateIdleTimer();
    return true;
}

QStringList DrmHost::arguments() const
{
    QStringList arguments = m_options.command ? m_options.command->arguments : QStringList();
    arguments << QStringLiteral("--noctara-user-data=%1").arg(m_options.profileDir)
              << QStringLiteral("--ozone-platform=headless");
    if (m_options.gpu) {
        arguments << QStringLiteral("--noctara-gpu");
    } else {
        arguments << QStringLiteral("--disable-gpu");
    }
    arguments << m_network.switches;
    return arguments;
}

void DrmHost::write(const QJsonObject& message)
{
    // Never logged: "load" carries the license token, "configure" the proxy credentials.
    if (m_process != nullptr && !m_stopping) {
        m_process->write(encode(message));
    }
}

void DrmHost::onStdout()
{
    if (m_process == nullptr) {
        return;
    }
    m_stdout += m_process->readAllStandardOutput();
    qsizetype newline = 0;
    while ((newline = m_stdout.indexOf('\n')) >= 0) {
        const QByteArray line = m_stdout.left(newline);
        m_stdout.remove(0, newline + 1);
        Result<QJsonObject> message = decode(line, Direction::FromHelper);
        if (!message) {
            qCWarning(lcDrm).noquote() << "ignored a message from the protected player:"
                                       << QString::fromStdString(message.error().detail);
            continue;
        }
        onMessage(*message);  // may restart or stop the helper, which clears m_stdout
    }
    if (m_stdout.size() > kMaxLine) {
        qCWarning(lcDrm) << "dropped an overlong line from the protected player";
        m_stdout.clear();
    }
}

void DrmHost::onStderr()
{
    if (m_process == nullptr) {
        return;
    }
    // Chromium's own diagnostics: debug output only.
    m_stderr += m_process->readAllStandardError();
    qsizetype newline = 0;
    while ((newline = m_stderr.indexOf('\n')) >= 0) {
        const QByteArray line = m_stderr.left(std::min<qsizetype>(newline, 400)).trimmed();
        m_stderr.remove(0, newline + 1);
        if (!line.isEmpty()) {
            qCDebug(lcDrm).noquote() << "helper:" << QString::fromUtf8(line);
        }
    }
    if (m_stderr.size() > kMaxLine) {
        m_stderr.clear();
    }
}

void DrmHost::onMessage(const QJsonObject& message)
{
    m_lastPong.restart();  // anything it says proves it is alive
    const QString type = message.value(u"type").toString();
    if (type == u"pong") {
        return;
    }
    if (type == u"ready") {
        m_startTimer.stop();
        const int version = message.value(u"version").toInt();
        if (version != protocolVersion()) {
            giveUp(Error{ErrorCode::ProtectedPlayerFailed,
                         QStringLiteral("protocol %1, expected %2").arg(version).arg(protocolVersion()).toStdString()});
            return;
        }
        if (!message.value(u"widevine").toBool()) {
            giveUp(Error{ErrorCode::ProtectedPlayerFailed,
                         "Widevine is unavailable: " + message.value(u"detail").toString().toStdString()});
            return;
        }
        m_electronVersion = message.value(u"electron").toString();
        m_error = Error{};
        NOCTARA_TRACE_INSTANT("drm: helper ready");
        qCInfo(lcDrm).noquote() << "protected player ready, Electron" << m_electronVersion;
        m_lastPong.restart();
        setStatus(Status::Ready);
        Q_EMIT ready();
        return;
    }
    if (type == u"log") {
        const QString level = message.value(u"level").toString();
        const QString text = message.value(u"message").toString();
        if (level == u"error" || level == u"warning") {
            qCWarning(lcDrm).noquote() << "helper:" << text;
        } else {
            qCDebug(lcDrm).noquote() << "helper:" << text;
        }
    }
    Q_EMIT received(message);
}

void DrmHost::heartbeat()
{
    if (!running()) {
        m_heartbeatTimer.stop();
        return;
    }
    if (m_status != Status::Ready) {
        return;  // starting: the start timeout applies (the first start may download the Widevine CDM)
    }
    if (m_lastPong.elapsed() > m_options.heartbeatTimeout.count()) {
        qCWarning(lcDrm) << "the protected player stopped answering; restarting it";
        killGroup();  // the exit is reported as a crash
        return;
    }
    m_pingSeq += 1;
    write(QJsonObject{{QStringLiteral("type"), QStringLiteral("ping")}, {QStringLiteral("seq"), m_pingSeq}});
}

void DrmHost::onFinished()
{
    const bool unexpected = !m_stopping;
    if (unexpected) {
        qCWarning(lcDrm) << "the protected player exited unexpectedly, code" << m_process->exitCode();
    }
    reap(unexpected);
    if (unexpected) {
        scheduleRestart();
    }
}

void DrmHost::terminateNow()
{
    if (m_process == nullptr) {
        return;
    }
    m_process->disconnect(this);
    killGroup();
    (void)m_process->waitForFinished(2000);
}

void DrmHost::reap(bool unexpected)
{
    killGroup();  // children it left behind
    m_heartbeatTimer.stop();
    m_startTimer.stop();
    m_killTimer.stop();
    m_idleTimer.stop();
    if (m_process != nullptr) {
        m_process->disconnect(this);
        m_process->deleteLater();  // we may be inside one of its signals
        m_process = nullptr;
    }
    m_pid = 0;
    m_stopping = false;
    m_stdout.clear();
    m_stderr.clear();
    if (m_status != Status::Failed) {
        setStatus(Status::Stopped);
    }
    Q_EMIT stopped(unexpected);
}

void DrmHost::scheduleRestart()
{
    QElapsedTimer now;
    now.start();
    m_crashes.push_back(now);
    while (!m_crashes.empty() && m_crashes.front().elapsed() > std::chrono::milliseconds(kCrashWindow).count()) {
        m_crashes.pop_front();
    }
    if (std::ssize(m_crashes) >= kMaxCrashes) {
        giveUp(Error{ErrorCode::ProtectedPlayerFailed, "the helper keeps crashing"});
        return;
    }
    if (m_busy) {
        const auto delay = m_options.restartBackoff * (std::size_t{1} << (m_crashes.size() - 1));
        qCInfo(lcDrm) << "restarting the protected player in" << delay.count() << "ms";
        m_restartTimer.start(delay);
    }
}

void DrmHost::giveUp(const Error& error)
{
    qCWarning(lcDrm).noquote() << "protected playback unavailable:" << QString::fromStdString(error.describe());
    m_restartTimer.stop();
    m_error = error;
    setStatus(Status::Failed);
    if (m_process != nullptr && m_process->state() != QProcess::NotRunning) {
        m_stopping = true;  // an exit Noctara caused
        killGroup();
    }
    Q_EMIT failed(error);
}

void DrmHost::killGroup() const
{
    if (m_pid > 0) {
        (void)::kill(static_cast<pid_t>(-m_pid), SIGKILL);
    }
}

void DrmHost::setStatus(Status status)
{
    if (status != m_status) {
        m_status = status;
        Q_EMIT statusChanged();
    }
}

void DrmHost::updateIdleTimer()
{
    if (running() && !m_busy) {
        if (!m_idleTimer.isActive()) {
            m_idleTimer.start(m_options.idleTimeout);
        }
    } else {
        m_idleTimer.stop();
    }
}

}  // namespace noctara::drm
