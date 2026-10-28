#pragma once

#include <QColor>
#include <QList>
#include <QQuickItem>

#include <vector>

namespace noctara::ui {

// Bar heights of a waveform resampled to `count` bars (each the peak of its share of `samples`, at least `floor`).
[[nodiscard]] std::vector<float> waveformBars(const QList<float>& samples, int count, float floor);
// Whether each bar's centre lies in one of the stored ranges ([from0, to0, from1, to1, …] fractions).
[[nodiscard]] std::vector<bool> storedBars(const QList<qreal>& ranges, int count);

// The waveform scrubber's drawing (QML WaveformView adds the interaction). With hardware rendering the whole
// waveform is one geometry node whose material colours played, stored, unplayed and hovered bars from uniforms,
// so playback and hovering only change uniforms. With the software renderer it is two cached images (played and
// not played) shown side by side, so playback only moves the split. Geometry and images are rebuilt only when
// the size, the samples, the stored ranges or the colours change; never per frame.
class WaveformItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QList<float> samples READ samples WRITE setSamples NOTIFY samplesChanged)
    Q_PROPERTY(qreal progress READ progress WRITE setProgress NOTIFY progressChanged)
    Q_PROPERTY(qreal hoverProgress READ hoverProgress WRITE setHoverProgress NOTIFY hoverProgressChanged)
    Q_PROPERTY(QList<qreal> storedRanges READ storedRanges WRITE setStoredRanges NOTIFY storedRangesChanged)
    Q_PROPERTY(QColor playedColor READ playedColor WRITE setPlayedColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor unplayedColor READ unplayedColor WRITE setUnplayedColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor storedColor READ storedColor WRITE setStoredColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor hoverColor READ hoverColor WRITE setHoverColor NOTIFY colorsChanged)
    Q_PROPERTY(qreal barWidth READ barWidth WRITE setBarWidth NOTIFY layoutChanged)
    Q_PROPERTY(qreal barGap READ barGap WRITE setBarGap NOTIFY layoutChanged)

public:
    explicit WaveformItem(QQuickItem* parent = nullptr);

    [[nodiscard]] const QList<float>& samples() const noexcept { return m_samples; }
    void setSamples(const QList<float>& samples);
    [[nodiscard]] qreal progress() const noexcept { return m_progress; }
    void setProgress(qreal progress);
    [[nodiscard]] qreal hoverProgress() const noexcept { return m_hover; }
    void setHoverProgress(qreal progress);
    [[nodiscard]] const QList<qreal>& storedRanges() const noexcept { return m_stored; }
    void setStoredRanges(const QList<qreal>& ranges);
    [[nodiscard]] QColor playedColor() const { return m_played; }
    void setPlayedColor(const QColor& color);
    [[nodiscard]] QColor unplayedColor() const { return m_unplayed; }
    void setUnplayedColor(const QColor& color);
    [[nodiscard]] QColor storedColor() const { return m_storedColor; }
    void setStoredColor(const QColor& color);
    [[nodiscard]] QColor hoverColor() const { return m_hoverColor; }
    void setHoverColor(const QColor& color);
    [[nodiscard]] qreal barWidth() const noexcept { return m_barWidth; }
    void setBarWidth(qreal width);
    [[nodiscard]] qreal barGap() const noexcept { return m_barGap; }
    void setBarGap(qreal gap);

    static constexpr float kFloor = 0.08F;  // the lowest bar, so silence still shows the track

Q_SIGNALS:
    void samplesChanged();
    void progressChanged();
    void hoverProgressChanged();
    void storedRangesChanged();
    void colorsChanged();
    void layoutChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF& now, const QRectF& before) override;

private:
    [[nodiscard]] int barCount() const;
    void invalidateShape();
    void setColor(QColor& member, const QColor& color);
    QSGNode* updateHardware(QSGNode* old);
    QSGNode* updateSoftware(QSGNode* old);

    QList<float> m_samples;
    QList<qreal> m_stored;
    qreal m_progress = 0;
    qreal m_hover = -1;
    QColor m_played;
    QColor m_unplayed;
    QColor m_storedColor;
    QColor m_hoverColor;
    qreal m_barWidth = 3;
    qreal m_barGap = 2;
    bool m_shapeDirty = true;   // geometry (hardware) or images (software) must be rebuilt
    bool m_colorsDirty = true;  // software images depend on the colours
};

}  // namespace noctara::ui
