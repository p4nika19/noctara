#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QThreadPool>

#include <functional>

namespace noctara::cache {
class ImageCache;
}

namespace noctara::media {

// Local copies of cover art for other processes (MPRIS clients, notifications). They must not fetch artwork
// themselves: that would bypass Noctara's DNS and proxy settings and its caches. Images come from the image cache,
// are written off the GUI thread into a private directory (the runtime directory), and are deleted on exit.
class CoverFiles : public QObject {
    Q_OBJECT

public:
    CoverFiles(cache::ImageCache& images, QString directory, QObject* parent = nullptr);
    ~CoverFiles() override;
    CoverFiles(const CoverFiles&) = delete;
    CoverFiles& operator=(const CoverFiles&) = delete;
    CoverFiles(CoverFiles&&) = delete;
    CoverFiles& operator=(CoverFiles&&) = delete;

    // `done` runs on this object's thread with the file path, or an empty string when there is no image.
    void request(const QString& url, std::function<void(const QString& path)> done);

    static constexpr int kKeep = 4;  // files kept (current track, recent ones)
    static constexpr int kSide = 512;

private:
    cache::ImageCache& m_images;
    QString m_directory;
    QHash<QString, QString> m_files;  // url → path
    QStringList m_order;              // oldest first
    QThreadPool m_writer;
};

}  // namespace noctara::media
