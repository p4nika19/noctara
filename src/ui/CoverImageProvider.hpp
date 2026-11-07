#pragma once

#include <QImage>
#include <QQuickAsyncImageProvider>
#include <QSize>

namespace noctara::cache {
class ImageCache;
}

namespace noctara::ui {

// image://cover/<percent-encoded URL> — cover art and avatars through the image cache (RAM, disk, then the
// network transport), decoded off the GUI thread at the requested size.
// image://cover/mask/<fraction>/<percent-encoded URL> additionally rounds the corners (radius = fraction of the
// shorter side, 0.5 = circle) off the GUI thread, which also works without shader effects.
// Rounds the corners of a cover on the CPU, so covers have the same shape with and without shader effects.
// `fraction` is the corner radius relative to the shorter side (0.5: a circle); the image is first cropped to the
// shape of `box`, the size the item shows it at.
[[nodiscard]] QImage maskedCover(const QImage& source, double fraction, QSize box);

class CoverImageProvider : public QQuickAsyncImageProvider {
public:
    explicit CoverImageProvider(cache::ImageCache& cache);
    QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requestedSize) override;

private:
    cache::ImageCache& m_cache;
};

}  // namespace noctara::ui
