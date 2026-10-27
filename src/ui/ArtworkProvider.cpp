#include "ui/ArtworkProvider.hpp"

#include "theme/AccentEngine.hpp"

namespace noctara::ui {

ArtworkProvider::ArtworkProvider(const theme::AccentEngine& engine)
    : QQuickImageProvider(QQuickImageProvider::Image)
    , m_engine(engine)
{
}

QImage ArtworkProvider::requestImage(const QString& id, QSize* size, const QSize& /*requestedSize*/)
{
    const QImage image = m_engine.backdrop(id.startsWith(u"light") ? theme::Scheme::Light : theme::Scheme::Dark);
    if (size != nullptr) {
        *size = image.size();
    }
    return image;
}

}  // namespace noctara::ui
