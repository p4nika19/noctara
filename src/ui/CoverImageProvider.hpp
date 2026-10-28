#pragma once

#include <QQuickAsyncImageProvider>

namespace noctara::cache {
class ImageCache;
}

namespace noctara::ui {

// image://cover/<percent-encoded URL> — cover art and avatars through the image cache (RAM, disk, then the
// network transport), decoded off the GUI thread at the requested size.
// image://cover/mask/<fraction>/<percent-encoded URL> additionally rounds the corners (radius = fraction of the
// shorter side, 0.5 = circle) off the GUI thread, which also works without shader effects.
class CoverImageProvider : public QQuickAsyncImageProvider {
public:
    explicit CoverImageProvider(cache::ImageCache& cache);
    QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requestedSize) override;

private:
    cache::ImageCache& m_cache;
};

}  // namespace noctara::ui
