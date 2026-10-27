#pragma once

#include <QQuickImageProvider>

namespace noctara::theme {
class AccentEngine;
}

namespace noctara::ui {

// image://artwork/<dark|light>/<revision> — the pre-blurred cover-art backdrop for a scheme, produced off the
// GUI thread by the AccentEngine. The revision only makes each new artwork a distinct URL.
class ArtworkProvider : public QQuickImageProvider {
public:
    explicit ArtworkProvider(const theme::AccentEngine& engine);
    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

private:
    const theme::AccentEngine& m_engine;
};

}  // namespace noctara::ui
