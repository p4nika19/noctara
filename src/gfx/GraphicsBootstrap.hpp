#pragma once

#include "gfx/GraphicsPolicy.hpp"

#include <QObject>
#include <QQuickWindow>
#include <QStringList>

namespace noctara::settings {
class SettingsStore;
}

namespace noctara::gfx {

class CrashSentinel;

struct BootstrapOptions {
    QString cacheDir;
    bool safeModeFlag = false;
    QString blocklistResource = QStringLiteral(":/data/gpu-blocklist.json");
};

// Runs once, after QGuiApplication and before the first window: chooses hardware vs software rendering
// and the graphics API, applies it to Qt Quick (QQuickWindow::setGraphicsApi, default surface format,
// GPU-selection environment), and arms the crash sentinel.
[[nodiscard]] GraphicsDecision bootstrapGraphics(const settings::SettingsStore& settings, CrashSentinel& sentinel,
                                                 const BootstrapOptions& options);

// QML-facing description of the active graphics configuration (Settings → Performance, About).
class GraphicsInfo : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hardware READ hardware CONSTANT)
    Q_PROPERTY(bool safeMode READ safeMode CONSTANT)
    Q_PROPERTY(QString api READ api CONSTANT)
    Q_PROPERTY(QString deviceName READ deviceName CONSTANT)
    Q_PROPERTY(QString deviceKind READ deviceKind CONSTANT)
    Q_PROPERTY(QString driver READ driver CONSTANT)
    Q_PROPERTY(int reason READ reason CONSTANT)
    Q_PROPERTY(bool liquidGlassAllowed READ liquidGlassAllowed CONSTANT)
    // Whether normalized device coordinates have Y pointing up (OpenGL) rather than down (Vulkan). Shaders that
    // map vertex positions back to window coordinates need it.
    Q_PROPERTY(bool ndcYUp READ ndcYUp CONSTANT)
    Q_PROPERTY(int adapterCount READ adapterCount CONSTANT)
    Q_PROPERTY(bool noticePending READ noticePending NOTIFY noticePendingChanged)
    Q_PROPERTY(bool deviceFailed READ deviceFailed NOTIFY deviceFailedChanged)

public:
    explicit GraphicsInfo(GraphicsDecision decision, QObject* parent = nullptr);

    [[nodiscard]] const GraphicsDecision& decision() const noexcept { return m_decision; }
    [[nodiscard]] bool hardware() const noexcept { return m_decision.hardware; }
    [[nodiscard]] bool safeMode() const noexcept { return m_decision.safeMode; }
    [[nodiscard]] QString api() const;
    [[nodiscard]] QString deviceName() const;
    [[nodiscard]] QString deviceKind() const;
    [[nodiscard]] QString driver() const;
    [[nodiscard]] int reason() const noexcept { return static_cast<int>(m_decision.reason); }
    [[nodiscard]] bool liquidGlassAllowed() const noexcept { return m_decision.liquidGlassAllowed; }
    [[nodiscard]] bool ndcYUp() const noexcept { return m_decision.hardware && m_decision.api != Api::Vulkan; }
    [[nodiscard]] int adapterCount() const;
    [[nodiscard]] bool noticePending() const noexcept { return m_noticePending; }
    [[nodiscard]] bool deviceFailed() const noexcept { return m_deviceFailed; }

    Q_INVOKABLE void acknowledgeNotice();

    // Must be connected for every QQuickWindow: an unhandled sceneGraphError aborts the process.
    void watch(QQuickWindow* window);

Q_SIGNALS:
    void noticePendingChanged();
    void deviceFailedChanged();
    // Emitted when rendering cannot continue on the GPU; the app should switch to software and restart.
    void graphicsFailed(const QString& message);

private:
    GraphicsDecision m_decision;
    bool m_noticePending = false;
    bool m_deviceFailed = false;
};

}  // namespace noctara::gfx
