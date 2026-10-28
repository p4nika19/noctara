#pragma once

#include <QQuickAsyncImageProvider>

namespace noctara::cache {
class ImageCache;
}

namespace noctara::ui {

// image://cover/<percent-encoded URL> — cover art and avatars through the image cache (RAM, disk, then the
// network transport), decoded off the GUI thread at the requested size.
class CoverImageProvider : public QQuickAsyncImageProvider {
public:
    explicit CoverImageProvider(cache::ImageCache& cache);
    QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requestedSize) override;

private:
    cache::ImageCache& m_cache;
};

}  // namespace noctara::ui
