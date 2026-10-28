#include "ui/CoverImageProvider.hpp"

#include "cache/ImageCache.hpp"

#include <QUrl>

namespace noctara::ui {

namespace {

class CoverResponse : public QQuickImageResponse {
public:
    void deliver(QImage image)
    {
        m_image = std::move(image);
        Q_EMIT finished();
    }
    void failed(const QString& reason)
    {
        m_error = reason;
        Q_EMIT finished();
    }
    [[nodiscard]] QQuickTextureFactory* textureFactory() const override
    {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }
    [[nodiscard]] QString errorString() const override { return m_error; }

private:
    QImage m_image;
    QString m_error;
};

}  // namespace

CoverImageProvider::CoverImageProvider(cache::ImageCache& cache)
    : m_cache(cache)
{
}

QQuickImageResponse* CoverImageProvider::requestImageResponse(const QString& id, const QSize& requestedSize)
{
    auto* response = new CoverResponse;  // NOLINT(cppcoreguidelines-owning-memory): owned by the QML engine
    const QUrl url(QUrl::fromPercentEncoding(id.toUtf8()));
    m_cache.request(url, requestedSize, response, [response](Result<QImage> image) {
        if (image) {
            response->deliver(std::move(*image));
        } else {
            response->failed(QString::fromStdString(image.error().describe()));
        }
    });
    return response;
}

}  // namespace noctara::ui
