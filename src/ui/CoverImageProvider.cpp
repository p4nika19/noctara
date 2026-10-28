#include "ui/CoverImageProvider.hpp"

#include "cache/ImageCache.hpp"

#include <QPainter>
#include <QUrl>

#include <algorithm>
#include <atomic>

namespace noctara::ui {

namespace {

// Finishes exactly once: with the image, with an error, or when the engine cancels it.
class CoverResponse : public QQuickImageResponse {
public:
    void deliver(QImage image)
    {
        if (claim()) {
            m_image = std::move(image);
            Q_EMIT finished();
        }
    }
    void failed(const QString& reason)
    {
        if (claim()) {
            m_error = reason;
            Q_EMIT finished();
        }
    }
    // The engine no longer wants the image (the item went away, or the engine is being destroyed). It deletes a
    // response only after finished(), so a cancelled one must finish now, not when the cache answers: otherwise
    // it leaks and the cache's answer later targets an object whose image-loader thread is gone.
    void cancel() override
    {
        if (claim()) {
            m_error = QStringLiteral("cancelled");  // never shown: the engine ignores a cancelled response
            Q_EMIT finished();
        }
    }
    [[nodiscard]] QQuickTextureFactory* textureFactory() const override
    {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }
    [[nodiscard]] QString errorString() const override { return m_error; }

private:
    bool claim() noexcept { return !m_finished.exchange(true); }

    std::atomic<bool> m_finished{false};  // deliver()/failed() and cancel() may run on different threads
    QImage m_image;
    QString m_error;
};

// Rounds the corners on the CPU (this runs on Qt's image loader thread), so covers have the same shape with and
// without shader effects. `fraction` is the corner radius relative to the shorter side; 0.5 is a circle.
QImage masked(const QImage& image, double fraction)
{
    if (fraction <= 0 || image.isNull()) {
        return image;
    }
    QImage result(image.size(), QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QBrush(image));
    const qreal radius = std::min(fraction, 0.5) * std::min(image.width(), image.height());
    painter.drawRoundedRect(QRectF(result.rect()), radius, radius);
    painter.end();
    return result;
}

}  // namespace

CoverImageProvider::CoverImageProvider(cache::ImageCache& cache)
    : m_cache(cache)
{
}

QQuickImageResponse* CoverImageProvider::requestImageResponse(const QString& id, const QSize& requestedSize)
{
    auto* response = new CoverResponse;  // NOLINT(cppcoreguidelines-owning-memory): owned by the QML engine
    QStringView encoded = id;
    double fraction = 0;
    if (encoded.startsWith(u"mask/")) {
        encoded = encoded.sliced(5);
        const qsizetype slash = encoded.indexOf(u'/');
        fraction = slash > 0 ? encoded.first(slash).toDouble() : 0;
        encoded = encoded.sliced(slash + 1);
    }
    const QUrl url(QUrl::fromPercentEncoding(encoded.toUtf8()));
    m_cache.request(url, requestedSize, response, [response, fraction](Result<QImage> image) {
        if (image) {
            response->deliver(masked(*image, fraction));
        } else {
            response->failed(QString::fromStdString(image.error().describe()));
        }
    });
    return response;
}

}  // namespace noctara::ui
