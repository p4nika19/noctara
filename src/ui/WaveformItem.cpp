#include "ui/WaveformItem.hpp"

#include <QImage>
#include <QPainter>
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGImageNode>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGRendererInterface>
#include <QSGTexture>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace noctara::ui {

namespace {

struct Vertex {
    float x, y;            // item coordinates
    float lx, ly, hw, hh;  // offset from the bar's centre and its half size
    float at, stored;      // fraction along the track, stored flag
};

const QSGGeometry::AttributeSet& vertexLayout()
{
    static const std::array attributes{
        QSGGeometry::Attribute::createWithAttributeType(0, 2, QSGGeometry::FloatType, QSGGeometry::PositionAttribute),
        QSGGeometry::Attribute::createWithAttributeType(1, 4, QSGGeometry::FloatType, QSGGeometry::UnknownAttribute),
        QSGGeometry::Attribute::createWithAttributeType(2, 2, QSGGeometry::FloatType, QSGGeometry::UnknownAttribute),
    };
    static const QSGGeometry::AttributeSet set{static_cast<int>(attributes.size()), sizeof(Vertex), attributes.data()};
    return set;
}

struct Uniforms {
    float progress = 0;
    float hover = -1;
    std::array<float, 4> played{};
    std::array<float, 4> unplayed{};
    std::array<float, 4> stored{};
    std::array<float, 4> hoverColor{};

    friend bool operator==(const Uniforms&, const Uniforms&) = default;
};

std::array<float, 4> straight(const QColor& color)
{
    return {color.redF(), color.greenF(), color.blueF(), color.alphaF()};
}

class WaveformMaterial : public QSGMaterial {
public:
    WaveformMaterial() { setFlag(Blending); }

    [[nodiscard]] QSGMaterialType* type() const override
    {
        static QSGMaterialType kind;
        return &kind;
    }
    [[nodiscard]] QSGMaterialShader* createShader(QSGRendererInterface::RenderMode /*mode*/) const override;
    [[nodiscard]] int compare(const QSGMaterial* other) const override
    {
        const auto* that = static_cast<const WaveformMaterial*>(other);
        return uniforms == that->uniforms ? 0 : (this < that ? -1 : 1);
    }

    Uniforms uniforms;
};

class WaveformShader : public QSGMaterialShader {
public:
    WaveformShader()
    {
        setShaderFileName(VertexStage, QStringLiteral(":/shaders/waveform.vert.qsb"));
        setShaderFileName(FragmentStage, QStringLiteral(":/shaders/waveform.frag.qsb"));
    }

    bool updateUniformData(RenderState& state, QSGMaterial* material, QSGMaterial* /*old*/) override
    {
        // std140: mat4 (64) · opacity, progress, hover, pad (16) · four vec4 colours (64).
        QByteArray* buffer = state.uniformData();
        char* data = buffer->data();
        if (state.isMatrixDirty()) {
            const QMatrix4x4 matrix = state.combinedMatrix();
            std::memcpy(data, matrix.constData(), 64);
        }
        if (state.isOpacityDirty()) {
            const float opacity = state.opacity();
            std::memcpy(data + 64, &opacity, sizeof(float));
        }
        const Uniforms& u = static_cast<WaveformMaterial*>(material)->uniforms;
        std::memcpy(data + 68, &u.progress, sizeof(float));
        std::memcpy(data + 72, &u.hover, sizeof(float));
        std::memcpy(data + 80, u.played.data(), 16);
        std::memcpy(data + 96, u.unplayed.data(), 16);
        std::memcpy(data + 112, u.stored.data(), 16);
        std::memcpy(data + 128, u.hoverColor.data(), 16);
        return true;
    }
};

QSGMaterialShader* WaveformMaterial::createShader(QSGRendererInterface::RenderMode /*mode*/) const
{
    return new WaveformShader;  // NOLINT(cppcoreguidelines-owning-memory): the scene graph takes ownership
}

// Software renderer: the played and the not-yet-played look of the waveform as two images.
class SoftwareWaveformNode : public QSGNode {
public:
    QSGImageNode* played = nullptr;
    QSGImageNode* rest = nullptr;
    QSizeF size;
};

}  // namespace

std::vector<float> waveformBars(const QList<float>& samples, int count, float floor)
{
    std::vector<float> bars(static_cast<std::size_t>(std::max(count, 0)), floor);
    if (samples.isEmpty() || count <= 0) {
        return bars;
    }
    const auto total = static_cast<double>(samples.size());
    for (int i = 0; i < count; ++i) {
        const auto first = static_cast<qsizetype>(std::floor(i * total / count));
        const auto last = std::max(first + 1, static_cast<qsizetype>(std::floor((i + 1) * total / count)));
        float peak = 0;
        for (qsizetype s = first; s < last && s < samples.size(); ++s) {
            peak = std::max(peak, samples[s]);
        }
        bars[static_cast<std::size_t>(i)] = std::clamp(peak, floor, 1.0F);
    }
    return bars;
}

std::vector<bool> storedBars(const QList<qreal>& ranges, int count)
{
    std::vector<bool> stored(static_cast<std::size_t>(std::max(count, 0)), false);
    for (int i = 0; i < count; ++i) {
        const double centre = (i + 0.5) / count;
        for (qsizetype r = 0; r + 1 < ranges.size(); r += 2) {
            if (centre >= ranges[r] && centre < ranges[r + 1]) {
                stored[static_cast<std::size_t>(i)] = true;
                break;
            }
        }
    }
    return stored;
}

WaveformItem::WaveformItem(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents);
}

int WaveformItem::barCount() const
{
    const qreal pitch = m_barWidth + m_barGap;
    return pitch > 0 && width() > m_barWidth ? static_cast<int>((width() + m_barGap) / pitch) : 0;
}

void WaveformItem::invalidateShape()
{
    m_shapeDirty = true;
    update();
}

void WaveformItem::setSamples(const QList<float>& samples)
{
    if (samples != m_samples) {
        m_samples = samples;
        invalidateShape();
        Q_EMIT samplesChanged();
    }
}

void WaveformItem::setProgress(qreal progress)
{
    progress = std::clamp<qreal>(progress, 0, 1);
    if (!qFuzzyCompare(progress + 1, m_progress + 1)) {
        m_progress = progress;
        update();
        Q_EMIT progressChanged();
    }
}

void WaveformItem::setHoverProgress(qreal progress)
{
    if (!qFuzzyCompare(progress + 2, m_hover + 2)) {
        m_hover = progress;
        update();
        Q_EMIT hoverProgressChanged();
    }
}

void WaveformItem::setStoredRanges(const QList<qreal>& ranges)
{
    if (ranges != m_stored) {
        m_stored = ranges;
        invalidateShape();
        Q_EMIT storedRangesChanged();
    }
}

void WaveformItem::setColor(QColor& member, const QColor& color)
{
    if (color != member) {
        member = color;
        m_colorsDirty = true;
        update();
        Q_EMIT colorsChanged();
    }
}

void WaveformItem::setPlayedColor(const QColor& color)
{
    setColor(m_played, color);
}

void WaveformItem::setUnplayedColor(const QColor& color)
{
    setColor(m_unplayed, color);
}

void WaveformItem::setStoredColor(const QColor& color)
{
    setColor(m_storedColor, color);
}

void WaveformItem::setHoverColor(const QColor& color)
{
    setColor(m_hoverColor, color);
}

void WaveformItem::setBarWidth(qreal width)
{
    if (width > 0 && !qFuzzyCompare(width, m_barWidth)) {
        m_barWidth = width;
        invalidateShape();
        Q_EMIT layoutChanged();
    }
}

void WaveformItem::setBarGap(qreal gap)
{
    if (gap >= 0 && !qFuzzyCompare(gap + 1, m_barGap + 1)) {
        m_barGap = gap;
        invalidateShape();
        Q_EMIT layoutChanged();
    }
}

void WaveformItem::geometryChange(const QRectF& now, const QRectF& before)
{
    QQuickItem::geometryChange(now, before);
    if (now.size() != before.size()) {
        invalidateShape();
    }
}

QSGNode* WaveformItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData* /*data*/)
{
    if (barCount() == 0 || height() <= 0) {
        delete old;  // NOLINT(cppcoreguidelines-owning-memory): the item owns the node it returned
        m_shapeDirty = true;
        return nullptr;
    }
    const bool software = window()->rendererInterface()->graphicsApi() == QSGRendererInterface::Software;
    return software ? updateSoftware(old) : updateHardware(old);
}

QSGNode* WaveformItem::updateHardware(QSGNode* old)
{
    auto* node = static_cast<QSGGeometryNode*>(old);
    if (node == nullptr) {
        node = new QSGGeometryNode;  // NOLINT(cppcoreguidelines-owning-memory): returned to the scene graph
        auto* geometry = new QSGGeometry(vertexLayout(), 0);  // NOLINT(cppcoreguidelines-owning-memory)
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        node->setMaterial(new WaveformMaterial);  // NOLINT(cppcoreguidelines-owning-memory)
        node->setFlag(QSGNode::OwnsMaterial);
        m_shapeDirty = true;
    }
    if (m_shapeDirty) {
        const int count = barCount();
        const std::vector<float> bars = waveformBars(m_samples, count, kFloor);
        const std::vector<bool> stored = storedBars(m_stored, count);
        QSGGeometry* geometry = node->geometry();
        geometry->allocate(count * 6);
        auto* vertices = static_cast<Vertex*>(geometry->vertexData());
        const auto halfWidth = static_cast<float>(m_barWidth / 2);
        const auto middle = static_cast<float>(height() / 2);
        const float aa = 1.0F;  // room for the antialiased edge
        for (int i = 0; i < count; ++i) {
            const auto centre = static_cast<float>((i * (m_barWidth + m_barGap)) + (m_barWidth / 2));
            const float halfHeight = std::max(bars[static_cast<std::size_t>(i)] * middle, halfWidth);
            const float at = (static_cast<float>(i) + 0.5F) / static_cast<float>(count);
            const float isStored = stored[static_cast<std::size_t>(i)] ? 1.0F : 0.0F;
            const auto corner = [&](float dx, float dy) {
                return Vertex{centre + dx, middle + dy, dx, dy, halfWidth, halfHeight, at, isStored};
            };
            const float ex = halfWidth + aa;
            const float ey = halfHeight + aa;
            Vertex* quad = vertices + (static_cast<std::ptrdiff_t>(i) * 6);
            quad[0] = corner(-ex, -ey);
            quad[1] = corner(ex, -ey);
            quad[2] = corner(-ex, ey);
            quad[3] = corner(ex, -ey);
            quad[4] = corner(ex, ey);
            quad[5] = corner(-ex, ey);
        }
        node->markDirty(QSGNode::DirtyGeometry);
        m_shapeDirty = false;
    }
    auto* material = static_cast<WaveformMaterial*>(node->material());
    const Uniforms uniforms{.progress = static_cast<float>(m_progress),
                            .hover = static_cast<float>(m_hover),
                            .played = straight(m_played),
                            .unplayed = straight(m_unplayed),
                            .stored = straight(m_storedColor),
                            .hoverColor = straight(m_hoverColor)};
    if (!(uniforms == material->uniforms)) {
        material->uniforms = uniforms;
        node->markDirty(QSGNode::DirtyMaterial);
    }
    return node;
}

QSGNode* WaveformItem::updateSoftware(QSGNode* old)
{
    auto* node = static_cast<SoftwareWaveformNode*>(old);
    if (node == nullptr) {
        node = new SoftwareWaveformNode;  // NOLINT(cppcoreguidelines-owning-memory): returned to the scene graph
        node->played = window()->createImageNode();
        node->rest = window()->createImageNode();
        node->played->setOwnsTexture(true);
        node->rest->setOwnsTexture(true);
        node->appendChildNode(node->played);
        node->appendChildNode(node->rest);
        m_shapeDirty = true;
    }
    if (m_shapeDirty || m_colorsDirty) {
        const int count = barCount();
        const std::vector<float> bars = waveformBars(m_samples, count, kFloor);
        const std::vector<bool> stored = storedBars(m_stored, count);
        const qreal dpr = window()->effectiveDevicePixelRatio();
        const QSize pixels = (size() * dpr).toSize();
        const auto paint = [&](bool playedLook) {
            QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(dpr);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            const qreal middle = height() / 2;
            for (int i = 0; i < count; ++i) {
                const qreal halfHeight =
                    std::max(static_cast<qreal>(bars[static_cast<std::size_t>(i)]) * middle, m_barWidth / 2);
                const QColor color =
                    playedLook ? m_played : (stored[static_cast<std::size_t>(i)] ? m_storedColor : m_unplayed);
                painter.setBrush(color);
                painter.drawRoundedRect(
                    QRectF(i * (m_barWidth + m_barGap), middle - halfHeight, m_barWidth, halfHeight * 2),
                    m_barWidth / 2, m_barWidth / 2);
            }
            return image;
        };
        node->played->setTexture(window()->createTextureFromImage(paint(true)));
        node->rest->setTexture(window()->createTextureFromImage(paint(false)));
        node->size = size();
        m_shapeDirty = false;
        m_colorsDirty = false;
    }
    // Playback only moves the split between the two images. (No hover highlight without shaders.)
    const qreal split = m_progress * node->size.width();
    const QSizeF texture = node->played->texture()->textureSize();
    const qreal scale = node->size.width() > 0 ? texture.width() / node->size.width() : 1;
    node->played->setRect(QRectF(0, 0, split, node->size.height()));
    node->played->setSourceRect(QRectF(0, 0, split * scale, texture.height()));
    node->rest->setRect(QRectF(split, 0, node->size.width() - split, node->size.height()));
    node->rest->setSourceRect(QRectF(split * scale, 0, (node->size.width() - split) * scale, texture.height()));
    return node;
}

}  // namespace noctara::ui
