#include "ui/IconProvider.hpp"

#include "core/FileIo.hpp"
#include "core/Log.hpp"

#include <QColor>
#include <QPainter>
#include <QRegularExpression>
#include <QSvgRenderer>
#include <QUrlQuery>

namespace noctara::ui {

namespace {

constexpr int kDefaultSize = 24;

}  // namespace

IconProvider::IconProvider()
    : QQuickImageProvider(QQuickImageProvider::Image, QQmlImageProviderBase::ForceAsynchronousImageLoading)
{
}

QByteArray IconProvider::tintSvg(const QByteArray& svg, const QColor& color, double strokeWidth)
{
    QByteArray out = svg;
    const QByteArray rgb = color.name(QColor::HexRgb).toLatin1();
    out.replace("currentColor", rgb);
    static const QRegularExpression strokeWidthAttr(QStringLiteral("stroke-width=\"[0-9.]+\""));
    QString text = QString::fromUtf8(out);
    text.replace(strokeWidthAttr, QStringLiteral("stroke-width=\"%1\"").arg(strokeWidth));
    // Alpha is applied as group opacity so overlapping strokes do not double up.
    if (color.alpha() < 255) {
        text.replace(QStringLiteral("<svg"), QStringLiteral("<svg opacity=\"%1\"").arg(color.alphaF()));
    }
    return text.toUtf8();
}

QByteArray IconProvider::source(const QString& name)
{
    const QMutexLocker lock(&m_mutex);
    if (const auto it = m_sources.constFind(name); it != m_sources.constEnd()) {
        return *it;
    }
    static const QRegularExpression safeName(QStringLiteral("^[a-z0-9-]+$"));
    QByteArray svg;
    if (safeName.match(name).hasMatch()) {
        if (auto content = readWholeFile(QStringLiteral(":/icons/%1.svg").arg(name))) {
            svg = std::move(*content);
        }
    }
    if (svg.isEmpty()) {
        qCWarning(lcUi) << "unknown icon" << name;
    }
    m_sources.insert(name, svg);
    return svg;
}

QImage IconProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize)
{
    const qsizetype query = id.indexOf(u'?');
    const QString name = id.left(query);
    const QUrlQuery params(query >= 0 ? id.mid(query + 1) : QString());
    const QColor color = QColor::fromString(params.queryItemValue(QStringLiteral("c"), QUrl::FullyDecoded));
    bool strokeOk = false;
    const double stroke = params.queryItemValue(QStringLiteral("s")).toDouble(&strokeOk);

    const QSize target(requestedSize.width() > 0 ? requestedSize.width() : kDefaultSize,
                       requestedSize.height() > 0 ? requestedSize.height() : kDefaultSize);
    if (size != nullptr) {
        *size = target;
    }
    QImage image(target, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    const QByteArray svg = source(name);
    if (svg.isEmpty()) {
        return image;
    }
    QSvgRenderer renderer(tintSvg(svg, color.isValid() ? color : QColor(Qt::black), strokeOk ? stroke : 1.5));
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(target)));
    return image;
}

}  // namespace noctara::ui
