#include "gfx/GraphicsBootstrap.hpp"

#include "core/FileIo.hpp"
#include "core/Log.hpp"
#include "gfx/CrashSentinel.hpp"
#include "gfx/GpuProbe.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"

#include <QSGRendererInterface>
#include <QSurfaceFormat>

namespace noctara::gfx {

namespace keys = settings::keys;

namespace {

// GPU selection on hybrid systems must be in the environment before the first context/instance exists.
void applyGpuPreference(const QString& preference)
{
    if (qEnvironmentVariableIsSet("DRI_PRIME")) {
        return;  // an explicit user environment always wins
    }
    if (preference == u"discrete") {
        qputenv("DRI_PRIME", "1");  // Mesa PRIME offload
    } else if (preference == u"integrated") {
        qputenv("DRI_PRIME", "0");
    }
}

void applyToQtQuick(const GraphicsDecision& decision, const settings::SettingsStore& settings)
{
    if (!decision.hardware) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        return;
    }
    QQuickWindow::setGraphicsApi(decision.api == Api::Vulkan ? QSGRendererInterface::Vulkan
                                                             : QSGRendererInterface::OpenGL);
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setSwapInterval(settings.boolValue(keys::VSync) ? 1 : 0);
    format.setAlphaBufferSize(0);  // opaque window: the backdrop is drawn by the app
    QSurfaceFormat::setDefaultFormat(format);
}

}  // namespace

GraphicsDecision bootstrapGraphics(const settings::SettingsStore& settings, CrashSentinel& sentinel,
                                   const BootstrapOptions& options)
{
    applyGpuPreference(settings.stringValue(keys::GpuPreference));

    GraphicsRequest request;
    request.acceleration = parseAcceleration(settings.stringValue(keys::GpuAcceleration));
    request.api = parseApi(settings.stringValue(keys::GraphicsApi));
    request.safeModeFlag = options.safeModeFlag;
    request.crashSentinelFound = sentinel.previousLaunchCrashed();

    Blocklist blocklist;
    if (auto data = readWholeFile(options.blocklistResource)) {
        blocklist = parseBlocklist(*data);
    }

    ProbeCache cache(options.cacheDir + QStringLiteral("/gpu-probe.json"));
    const auto probe = [&cache](Api api) {
        if (auto cached = cache.lookup(api)) {
            return *cached;
        }
        ProbeResult result = probeRhi(api);
        cache.store(result);
        return result;
    };

    // The sentinel covers probing too: a driver that crashes while probing must not loop.
    sentinel.arm();
    GraphicsDecision decision = decide(request, probe, blocklist);
    cache.save();
    applyToQtQuick(decision, settings);
    qCInfo(lcGfx).noquote() << "rendering:" << (decision.hardware ? apiName(decision.api) : QStringLiteral("software"))
                            << (decision.safeMode ? "(safe mode)" : "") << '-' << decision.detail;
    return decision;
}

GraphicsInfo::GraphicsInfo(GraphicsDecision decision, QObject* parent)
    : QObject(parent)
    , m_decision(std::move(decision))
    , m_noticePending(m_decision.showNotice)
{
}

QString GraphicsInfo::api() const
{
    return m_decision.hardware ? apiName(m_decision.api) : QStringLiteral("software");
}

QString GraphicsInfo::deviceName() const
{
    return m_decision.probe ? m_decision.probe->deviceName : QString();
}

QString GraphicsInfo::deviceKind() const
{
    return m_decision.probe ? deviceKindName(m_decision.probe->kind) : QString();
}

QString GraphicsInfo::driver() const
{
    return m_decision.probe ? m_decision.probe->driver : QString();
}

int GraphicsInfo::adapterCount() const
{
    return m_decision.probe ? m_decision.probe->adapterCount : 0;
}

void GraphicsInfo::acknowledgeNotice()
{
    if (m_noticePending) {
        m_noticePending = false;
        Q_EMIT noticePendingChanged();
    }
}

void GraphicsInfo::watch(QQuickWindow* window)
{
    connect(window, &QQuickWindow::sceneGraphError, this,
            [this](QQuickWindow::SceneGraphError error, const QString& message) {
                qCCritical(lcGfx).noquote() << "scene graph error" << static_cast<int>(error) << message;
                if (!m_deviceFailed) {
                    m_deviceFailed = true;
                    Q_EMIT deviceFailedChanged();
                    Q_EMIT graphicsFailed(message);
                }
            });
}

}  // namespace noctara::gfx
