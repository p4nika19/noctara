#pragma once

#include <QQuickImageProvider>

namespace noctara::ui {

// image://noise/<size> — a deterministic, tileable grain texture (generated once, then cached by Qt's
// pixmap cache). Each texel is either a faint white or a faint black speck, so the grain averages out to no
// change in brightness. Tiled over the backdrop only (never over text) to break up gradient banding.
class NoiseProvider : public QQuickImageProvider {
public:
    NoiseProvider();
    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;
};

}  // namespace noctara::ui
