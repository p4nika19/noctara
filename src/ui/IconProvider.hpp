#pragma once

#include <QHash>
#include <QMutex>
#include <QQuickImageProvider>

namespace noctara::ui {

// image://icon/<name>?c=<#aarrggbb>&s=<stroke width>
// Rasterises the bundled stroke icons off the GUI thread at the exact requested size (sourceSize × DPR),
// tinted with a theme colour token. Qt's pixmap cache keys on the full URL, so each (icon, colour,
// stroke, size) is rendered once.
class IconProvider : public QQuickImageProvider {
public:
    IconProvider();

    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

    // Exposed for tests.
    [[nodiscard]] static QByteArray tintSvg(const QByteArray& svg, const QColor& color, double strokeWidth);

private:
    [[nodiscard]] QByteArray source(const QString& name);

    QMutex m_mutex;
    QHash<QString, QByteArray> m_sources;
};

}  // namespace noctara::ui
