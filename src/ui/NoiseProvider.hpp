#pragma once

#include <QQuickImageProvider>

namespace noctara::ui {

// image://noise/<size> — a deterministic, tileable greyscale grain texture (generated once, then cached
// by Qt's pixmap cache). Tiled across the window at low opacity to break up gradient banding.
class NoiseProvider : public QQuickImageProvider {
public:
    NoiseProvider();
    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;
};

}  // namespace noctara::ui
