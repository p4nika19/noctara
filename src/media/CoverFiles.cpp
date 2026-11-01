#include "media/CoverFiles.hpp"

#include "cache/Hash.hpp"
#include "cache/ImageCache.hpp"
#include "core/FileIo.hpp"
#include "core/Handoff.hpp"
#include "core/Log.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>

namespace noctara::media {

CoverFiles::CoverFiles(cache::ImageCache& images, QString directory, QObject* parent)
    : QObject(parent)
    , m_images(images)
    , m_directory(std::move(directory))
{
    m_writer.setMaxThreadCount(1);
    // Folders of earlier runs that ended without cleaning up (crash, SIGTERM): their process no longer exists.
    m_writer.start([directory = m_directory] {
        const QString name = directory.section(u'/', -1);
        const QString prefix = name.left(name.lastIndexOf(u'-') + 1);
        if (prefix.isEmpty()) {
            return;
        }
        const QDir parent(directory.section(u'/', 0, -2));
        for (const QString& entry : parent.entryList({prefix + u'*'}, QDir::Dirs | QDir::NoDotAndDotDot)) {
            bool numeric = false;
            const qint64 pid = entry.mid(prefix.size()).toLongLong(&numeric);
            if (numeric && entry != name && !QFileInfo::exists(QStringLiteral("/proc/%1").arg(pid))) {
                QDir(parent.filePath(entry)).removeRecursively();
            }
        }
    });
}

CoverFiles::~CoverFiles()
{
    m_writer.waitForDone();
    for (const QString& path : std::as_const(m_files)) {
        QFile::remove(path);
    }
    if (!m_directory.isEmpty()) {
        QDir().rmdir(m_directory);  // only when empty
    }
}

void CoverFiles::request(const QString& url, std::function<void(const QString& path)> done)
{
    if (url.isEmpty() || m_directory.isEmpty()) {
        done({});
        return;
    }
    if (const auto it = m_files.constFind(url); it != m_files.constEnd()) {
        done(*it);
        return;
    }
    m_images.request(QUrl(url), QSize(kSide, kSide), this, [this, url, done = std::move(done)](Result<QImage> image) {
        if (!image || image->isNull()) {
            done({});
            return;
        }
        const QString path = m_directory + QStringLiteral("/cover-") +
                             QString::fromLatin1(cache::contentHash(url.toUtf8()).left(16)) + QStringLiteral(".png");
        // The destructor waits for the writer, so `this` outlives the job; a posted answer is dropped with it.
        m_writer.start([this, directory = m_directory, path, url, image = *image, done] {
            const bool written = ensureDirectory(directory) && image.save(path, "PNG");
            if (!written) {
                qCInfo(lcMedia) << "could not write a cover file to" << directory;
            }
            handoff::post(this, [this, path, url, written, done] {
                if (!written) {
                    done({});
                    return;
                }
                // Requests for one URL made before its file was written each get here: track it once.
                if (!m_files.contains(url)) {
                    m_order.append(url);
                }
                m_files.insert(url, path);
                while (m_order.size() > kKeep) {
                    QFile::remove(m_files.take(m_order.takeFirst()));
                }
                done(path);
            });
        });
    });
}

}  // namespace noctara::media
