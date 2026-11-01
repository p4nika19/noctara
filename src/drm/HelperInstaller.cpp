#include "drm/HelperInstaller.hpp"

#include "core/FileIo.hpp"
#include "core/Handoff.hpp"
#include "core/Log.hpp"
#include "drm/HelperFiles.hpp"
#include "net/Transport.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <archive.h>
#include <archive_entry.h>
#include <memory>
#include <vector>

namespace noctara::drm {

namespace {

constexpr qint64 kReadChunk = qint64{1024} * 1024;
constexpr std::int64_t kProgressStep = std::int64_t{512} * 1024;  // report at most every half megabyte
constexpr std::size_t kArchiveBlock = std::size_t{64} * 1024;

QString folderName(const QString& version)
{
    QString name = version;
    for (QChar& c : name) {
        if (!c.isLetterOrNumber() && c != u'.' && c != u'-') {
            c = u'-';
        }
    }
    return name;
}

struct ArchiveReader {
    void operator()(archive* a) const { archive_read_free(a); }
};
struct ArchiveWriter {
    void operator()(archive* a) const { archive_write_free(a); }
};

Error archiveError(archive* a, const char* what)
{
    const char* detail = archive_error_string(a);
    return Error{ErrorCode::Corrupt, std::string(what) + ": " + (detail != nullptr ? detail : "unknown")};
}

}  // namespace

Result<void> extractZip(const QString& zip, const QString& into)
{
    const std::unique_ptr<archive, ArchiveReader> reader(archive_read_new());
    const std::unique_ptr<archive, ArchiveWriter> writer(archive_write_disk_new());
    if (!reader || !writer) {
        return fail(ErrorCode::IoError, "libarchive could not start");
    }
    archive_read_support_format_zip(reader.get());
    // Paths stay inside `into`: entry names are checked below (no absolute paths, no ".."), and nothing is
    // written through a symlink.
    archive_write_disk_set_options(writer.get(), ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                                                     ARCHIVE_EXTRACT_SECURE_SYMLINKS | ARCHIVE_EXTRACT_SECURE_NODOTDOT);
    archive_write_disk_set_standard_lookup(writer.get());
    if (archive_read_open_filename(reader.get(), QFile::encodeName(zip).constData(), kArchiveBlock) != ARCHIVE_OK) {
        return std::unexpected(archiveError(reader.get(), "cannot open the archive"));
    }
    // The folder's own path is resolved first: the symlink check covers every component of the targets, so a
    // symlink in the user's prefix (a linked home or data folder) would otherwise refuse every entry.
    if (!QDir().mkpath(into)) {
        return fail(ErrorCode::IoError, "cannot create " + into.toStdString());
    }
    const QString folder = QFileInfo(into).canonicalFilePath();
    if (folder.isEmpty()) {
        return fail(ErrorCode::IoError, "cannot resolve " + into.toStdString());
    }
    const QByteArray base = QFile::encodeName(folder) + '/';
    archive_entry* entry = nullptr;
    int status = ARCHIVE_OK;
    while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK) {
        const QByteArray name = archive_entry_pathname(entry);
        if (name.isEmpty() || name.startsWith('/') || name == ".." || name.startsWith("../") || name.contains("/../") ||
            name.endsWith("/..")) {
            return fail(ErrorCode::Corrupt, "archive entry outside its folder: " + name.toStdString());
        }
        const QByteArray target = base + name;
        archive_entry_set_pathname(entry, target.constData());
        if (archive_write_header(writer.get(), entry) != ARCHIVE_OK) {
            return std::unexpected(archiveError(writer.get(), QByteArray("cannot unpack " + name).constData()));
        }
        const void* block = nullptr;
        std::size_t size = 0;
        la_int64_t offset = 0;
        int read = ARCHIVE_OK;
        while ((read = archive_read_data_block(reader.get(), &block, &size, &offset)) == ARCHIVE_OK) {
            if (archive_write_data_block(writer.get(), block, size, offset) != ARCHIVE_OK) {
                return std::unexpected(archiveError(writer.get(), "cannot write an entry"));
            }
        }
        if (read != ARCHIVE_EOF) {
            return std::unexpected(archiveError(reader.get(), "cannot read an entry"));
        }
        if (archive_write_finish_entry(writer.get()) != ARCHIVE_OK) {
            return std::unexpected(archiveError(writer.get(), "cannot finish an entry"));
        }
    }
    if (status != ARCHIVE_EOF) {
        return std::unexpected(archiveError(reader.get(), "damaged archive"));
    }
    return {};
}

// One installation, on the installer's thread: downloads, checks, unpacks, writes the app, tidies up.
class InstallJob : public QObject {
public:
    InstallJob(HelperInstaller* owner, net::INetworkTransport& network, bool background)
        : m_owner(owner)
        , m_network(network)
        , m_background(background)
        , m_root(owner->root())
        , m_runtime(owner->runtimeDir())
        , m_app(owner->appDir())
        , m_downloads(owner->root() + QStringLiteral("/downloads"))
        , m_release(owner->release())
    {
    }

    void start()
    {
        const HelperRelease& release = m_release;
        if (!QFileInfo::exists(m_runtime + QStringLiteral("/.complete"))) {
            m_pending = {release.electron, release.hlsjs};
            m_total = release.electron.size + release.hlsjs.size;
        }
        if (Result<void> made = ensureDirectory(m_downloads); !made) {
            done(made.error());
            return;
        }
        next();
    }

    void cancel()
    {
        m_request.cancel();
        m_cancelled = true;
    }

private:
    void next()
    {
        if (m_cancelled) {
            return;
        }
        if (m_pending.empty()) {
            finishInstall();
            return;
        }
        const Artifact artifact = m_pending.front();
        const QString finished = m_downloads + u'/' + artifact.file;
        if (QFileInfo(finished).size() == artifact.size && sha256Of(finished) == artifact.sha256) {
            m_done += artifact.size;
            m_pending.erase(m_pending.begin());
            next();
            return;
        }
        startDownload(artifact);
    }

    // Resumes a partial download (its bytes are hashed again first) with a Range request.
    void startDownload(const Artifact& artifact)
    {
        const QString part = m_downloads + u'/' + artifact.file + QStringLiteral(".part");
        m_file = std::make_unique<QFile>(part);
        if (!m_file->open(QIODevice::ReadWrite)) {
            done(Error{ErrorCode::IoError, "cannot write " + part.toStdString()});
            return;
        }
        m_hash.reset();
        while (!m_file->atEnd()) {
            m_hash.addData(m_file->read(kReadChunk));
        }
        m_offset = m_file->size();
        // Every byte arrived last time and only the transfer did not finish: a Range past the end would be refused.
        if (m_offset == artifact.size && m_hash.result().toHex() == artifact.sha256) {
            m_file->close();
            keep(artifact);
            return;
        }
        if (m_offset >= artifact.size) {
            restartFile();
        }
        net::Request request;
        request.url = artifact.url;
        request.background = m_background;
        request.retry = false;  // a broken transfer resumes from what is on disk instead
        request.cache = net::CacheMode::Bypass;
        if (m_offset > 0) {
            request.range = net::ByteRange{.first = m_offset, .last = -1};
        }
        net::Callbacks callbacks;
        callbacks.onResponse = [this](const net::Response& response) {
            if (response.status == 200 && m_offset > 0) {
                restartFile();  // the server sent the whole file instead of the rest
            }
        };
        callbacks.onData = [this](const QByteArray& chunk) {
            if (m_file->write(chunk) != chunk.size()) {
                m_request.cancel();
                m_writeFailed = true;
                return;
            }
            m_hash.addData(chunk);
            m_offset += chunk.size();
            if (m_offset - m_reported >= kProgressStep) {
                m_reported = m_offset;
                report(HelperInstaller::Phase::Downloading);
            }
        };
        callbacks.onFinished = [this, artifact](Result<net::Response> response) {
            finishDownload(artifact, std::move(response));
        };
        report(HelperInstaller::Phase::Downloading);
        m_request = m_network.send(std::move(request), this, std::move(callbacks));
    }

    void restartFile()
    {
        (void)m_file->resize(0);
        (void)m_file->seek(0);
        m_hash.reset();
        m_offset = 0;
    }

    void finishDownload(const Artifact& artifact, Result<net::Response> response)
    {
        m_file->close();
        if (m_cancelled) {
            return;
        }
        if (m_writeFailed) {
            done(Error{ErrorCode::NoSpace, "cannot write the download"});
            return;
        }
        if (!response) {
            done(response.error());  // the part stays for the next attempt
            return;
        }
        if (!response->successful()) {
            done(Error{ErrorCode::HttpError,
                       "HTTP " + std::to_string(response->status) + " for " + artifact.url.toString().toStdString()});
            return;
        }
        const QString part = m_file->fileName();
        if (m_hash.result().toHex() != artifact.sha256) {
            (void)QFile::remove(part);
            done(Error{ErrorCode::Corrupt, artifact.file.toStdString() + " does not match its pinned SHA-256"});
            return;
        }
        keep(artifact);
    }

    // Moves a checked part into place and goes on with the next artifact.
    void keep(const Artifact& artifact)
    {
        const QString part = m_file->fileName();
        const QString finished = m_downloads + u'/' + artifact.file;
        (void)QFile::remove(finished);
        if (!QFile::rename(part, finished)) {
            done(Error{ErrorCode::IoError, "cannot keep " + finished.toStdString()});
            return;
        }
        m_done += artifact.size;
        m_offset = 0;
        m_reported = 0;
        m_pending.erase(m_pending.begin());
        next();
    }

    void finishInstall()
    {
        report(HelperInstaller::Phase::Unpacking);
        const HelperRelease& release = m_release;
        if (!QFileInfo::exists(m_runtime + QStringLiteral("/.complete"))) {
            const QString staging = m_runtime + QStringLiteral(".staging");
            (void)QDir(staging).removeRecursively();
            if (Result<void> made = ensureDirectory(staging); !made) {
                done(made.error());
                return;
            }
            if (Result<void> unpacked =
                    extractZip(m_downloads + u'/' + release.electron.file, staging + QStringLiteral("/electron"));
                !unpacked) {
                done(unpacked.error());
                return;
            }
            if (!QFile::copy(m_downloads + u'/' + release.hlsjs.file, staging + QStringLiteral("/hls.min.js"))) {
                done(Error{ErrorCode::IoError, "cannot copy hls.js"});
                return;
            }
            (void)QDir(m_runtime).removeRecursively();
            if (!QDir().rename(staging, m_runtime)) {
                done(Error{ErrorCode::IoError, "cannot move the runtime into place"});
                return;
            }
            if (Result<void> marked =
                    writeFileAtomic(m_runtime + QStringLiteral("/.complete"), release.electron.version.toUtf8());
                !marked) {
                done(marked.error());
                return;
            }
        }
        if (Result<void> written = writeApp(); !written) {
            done(written.error());
            return;
        }
        tidy();
        done(std::nullopt);
    }

    [[nodiscard]] Result<void> writeApp() const
    {
        if (QFileInfo::exists(m_app + QStringLiteral("/main.js"))) {
            return {};
        }
        const QString staging = m_app + QStringLiteral(".staging");
        (void)QDir(staging).removeRecursively();
        if (Result<void> made = ensureDirectory(staging + QStringLiteral("/vendor")); !made) {
            return made;
        }
        for (const EmbeddedFile& file : helperAppFiles()) {
            if (Result<void> written = writeFileAtomic(staging + u'/' + QString::fromLatin1(file.name), file.data, 0644,
                                                       Durability::Atomic);
                !written) {
                return written;
            }
        }
        if (!QFile::copy(m_runtime + QStringLiteral("/hls.min.js"), staging + QStringLiteral("/vendor/hls.min.js"))) {
            return fail(ErrorCode::IoError, "cannot copy hls.js into the app");
        }
        if (!QDir().rename(staging, m_app)) {
            return fail(ErrorCode::IoError, "cannot move the app into place");
        }
        return {};
    }

    // Older runtimes and apps, and the downloads, go.
    void tidy() const
    {
        const QDir root(m_root);
        for (const QFileInfo& entry : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString path = entry.absoluteFilePath();
            if (path != QFileInfo(m_runtime).absoluteFilePath() && path != QFileInfo(m_app).absoluteFilePath()) {
                (void)QDir(path).removeRecursively();
            }
        }
    }

    static QByteArray sha256Of(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        (void)hash.addData(&file);
        return hash.result().toHex();
    }

    void report(HelperInstaller::Phase phase)
    {
        handoff::post(m_owner, [owner = m_owner, phase, received = m_done + m_offset, total = m_total] {
            owner->onProgress(phase, received, total);
        });
    }

    // nullopt: installed. The installer deletes the job afterwards.
    void done(std::optional<Error> error)
    {
        handoff::post(m_owner, [owner = m_owner, error = std::move(error)] { owner->onFinished(error); });
    }

    HelperInstaller* m_owner;
    net::INetworkTransport& m_network;
    bool m_background;
    QString m_root;
    QString m_runtime;
    QString m_app;
    QString m_downloads;
    HelperRelease m_release;
    std::vector<Artifact> m_pending;
    std::unique_ptr<QFile> m_file;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    net::RequestHandle m_request;
    std::int64_t m_offset = 0;    // bytes of the current file on disk
    std::int64_t m_reported = 0;  // progress last reported at
    std::int64_t m_done = 0;      // bytes of finished files
    std::int64_t m_total = 0;
    bool m_writeFailed = false;
    bool m_cancelled = false;
};

HelperInstaller::HelperInstaller(net::INetworkTransport& network, QString dataDir, std::optional<HelperRelease> release,
                                 QObject* parent)
    : QObject(parent)
    , m_network(network)
    , m_root(std::move(dataDir) + QStringLiteral("/drm-helper"))
    , m_release(release ? *std::move(release) : helperRelease())
{
    m_thread.setObjectName(QStringLiteral("noctara-drm-install"));
}

HelperInstaller::~HelperInstaller()
{
    if (m_thread.isRunning()) {
        if (m_job != nullptr) {
            InstallJob* job = std::exchange(m_job, nullptr);
            handoff::runAndWait(job, [job] {
                job->cancel();
                delete job;  // NOLINT(cppcoreguidelines-owning-memory): owned by the installer, deleted on its thread
            });
        }
        m_thread.quit();
        m_thread.wait();
    }
}

QString HelperInstaller::runtimeDir() const
{
    return m_root + QStringLiteral("/electron-") + folderName(m_release.electron.version);
}

QString HelperInstaller::appDir() const
{
    return m_root + QStringLiteral("/app-") + helperAppId();
}

std::optional<HelperCommand> HelperInstaller::installed() const
{
    const QString electron = runtimeDir() + QStringLiteral("/electron/electron");
    if (!QFileInfo::exists(runtimeDir() + QStringLiteral("/.complete")) || !QFileInfo(electron).isExecutable() ||
        !QFileInfo::exists(appDir() + QStringLiteral("/main.js"))) {
        return std::nullopt;
    }
    return HelperCommand{.program = electron, .arguments = {appDir()}};
}

void HelperInstaller::install(bool background)
{
    if (running()) {
        return;
    }
    m_error = Error{};
    m_received = 0;
    m_total = 0;
    m_phase = Phase::Downloading;
    Q_EMIT progressChanged();
    if (!m_thread.isRunning()) {
        m_thread.start(QThread::LowPriority);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): owned by the installer (m_job), deleted on its thread
    auto* job = new InstallJob(this, m_network, background);
    job->moveToThread(&m_thread);
    m_job = job;
    qCInfo(lcDrm) << "setting up the protected player in" << m_root;
    handoff::post(job, [job] { job->start(); });
}

void HelperInstaller::onProgress(Phase phase, std::int64_t received, std::int64_t total)
{
    if (!running()) {
        return;
    }
    m_phase = phase;
    m_received = received;
    m_total = total;
    Q_EMIT progressChanged();
}

void HelperInstaller::onFinished(const std::optional<Error>& error)
{
    if (m_job != nullptr) {
        std::exchange(m_job, nullptr)->deleteLater();  // on its own thread
    }
    const bool ok = !error;
    m_phase = ok ? Phase::Done : Phase::Failed;
    m_error = error.value_or(Error{});
    if (ok) {
        qCInfo(lcDrm) << "protected player set up";
    } else {
        qCWarning(lcDrm).noquote() << "could not set up the protected player:"
                                   << QString::fromStdString(error->describe());
    }
    Q_EMIT progressChanged();
    Q_EMIT finished(ok);
}

}  // namespace noctara::drm
