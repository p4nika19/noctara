#include "ui/NoiseProvider.hpp"

#include <QRandomGenerator>

#include <algorithm>

namespace noctara::ui {

NoiseProvider::NoiseProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage NoiseProvider::requestImage(const QString& id, QSize* size, const QSize& /*requestedSize*/)
{
    bool ok = false;
    const int edge = std::clamp(id.toInt(&ok), 16, 512);
    const int side = ok ? edge : 128;
    QImage image(side, side, QImage::Format_ARGB32_Premultiplied);
    QRandomGenerator rng(0x6e6f6374);  // fixed seed: identical grain on every run
    for (int y = 0; y < side; ++y) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        for (int x = 0; x < side; ++x) {
            const auto value = static_cast<int>(rng.bounded(256U));
            line[x] = qRgba(value, value, value, 255);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        }
    }
    if (size != nullptr) {
        *size = image.size();
    }
    return image;
}

}  // namespace noctara::ui
