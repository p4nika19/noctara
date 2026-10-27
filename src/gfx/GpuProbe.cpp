#include "gfx/GpuProbe.hpp"

#include "core/FileIo.hpp"
#include "core/Log.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QOffscreenSurface>
#include <QVulkanFunctions>
#include <QVulkanInstance>

#include <memory>
#include <rhi/qrhi.h>

namespace noctara::gfx {

namespace {

DeviceKind kindFrom(QRhiDriverInfo::DeviceType type)
{
    switch (type) {
    case QRhiDriverInfo::IntegratedDevice: return DeviceKind::Integrated;
    case QRhiDriverInfo::DiscreteDevice: return DeviceKind::Discrete;
    case QRhiDriverInfo::VirtualDevice: return DeviceKind::Virtual;
    case QRhiDriverInfo::CpuDevice: return DeviceKind::Cpu;
    case QRhiDriverInfo::ExternalDevice:
    case QRhiDriverInfo::UnknownDevice: break;
    }
    return DeviceKind::Unknown;
}

void fillFromRhi(ProbeResult& result, const QRhi& rhi)
{
    const QRhiDriverInfo info = rhi.driverInfo();
    result.ok = true;
    result.deviceName = QString::fromUtf8(info.deviceName);
    result.vendorId = info.vendorId;
    result.deviceId = info.deviceId;
    result.kind = kindFrom(info.deviceType);
    if (isSoftwareRasterizer(result.deviceName)) {
        result.kind = DeviceKind::Cpu;
    }
}

ProbeResult probeVulkan(DevicePreference preference)
{
    ProbeResult result;
    result.api = Api::Vulkan;
    // These platform plugins cannot host a Vulkan instance; trying only logs Qt warnings.
    const QString platform = QGuiApplication::platformName();
    if (platform == u"offscreen" || platform == u"minimal") {
        result.error = QStringLiteral("the %1 platform has no Vulkan support").arg(platform);
        return result;
    }
    QVulkanInstance instance;
    instance.setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
    if (!instance.create()) {
        result.error = QStringLiteral("Vulkan instance creation failed (%1)").arg(instance.errorCode());
        return result;
    }
    QVulkanFunctions* functions = instance.functions();
    uint32_t count = 0;
    functions->vkEnumeratePhysicalDevices(instance.vkInstance(), &count, nullptr);
    result.adapterCount = static_cast<int>(count);

    // On hybrid systems pick the physical device by type. Qt's Vulkan backend honours
    // QT_VK_PHYSICAL_DEVICE_INDEX, both here and for the real window; an explicit user value always wins.
    if (preference != DevicePreference::Auto && count > 1 &&
        !qEnvironmentVariableIsSet("QT_VK_PHYSICAL_DEVICE_INDEX")) {
        std::vector<VkPhysicalDevice> devices(count);
        functions->vkEnumeratePhysicalDevices(instance.vkInstance(), &count, devices.data());
        std::vector<DeviceKind> kinds;
        for (VkPhysicalDevice device : devices) {
            VkPhysicalDeviceProperties properties{};
            functions->vkGetPhysicalDeviceProperties(device, &properties);
            kinds.push_back(properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? DeviceKind::Integrated
                            : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? DeviceKind::Discrete
                            : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU          ? DeviceKind::Cpu
                                                                                            : DeviceKind::Unknown);
        }
        result.deviceIndex = preferredDeviceIndex(kinds, preference);
        if (result.deviceIndex >= 0) {
            qputenv("QT_VK_PHYSICAL_DEVICE_INDEX", QByteArray::number(result.deviceIndex));
        }
    }

    QRhiVulkanInitParams params;
    params.inst = &instance;
    const std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::Vulkan, &params));
    if (!rhi) {
        result.error = QStringLiteral("QRhi Vulkan device creation failed");
        return result;
    }
    fillFromRhi(result, *rhi);
    VkPhysicalDeviceProperties properties{};
    const auto* native = static_cast<const QRhiVulkanNativeHandles*>(rhi->nativeHandles());
    if (native != nullptr && native->physDev != VK_NULL_HANDLE) {
        functions->vkGetPhysicalDeviceProperties(native->physDev, &properties);
        result.driver = QStringLiteral("Vulkan %1.%2.%3, driver 0x%4")
                            .arg(VK_API_VERSION_MAJOR(properties.apiVersion))
                            .arg(VK_API_VERSION_MINOR(properties.apiVersion))
                            .arg(VK_API_VERSION_PATCH(properties.apiVersion))
                            .arg(properties.driverVersion, 0, 16);
    }
    return result;
}

ProbeResult probeOpenGl()
{
    ProbeResult result;
    result.api = Api::OpenGL;
    const std::unique_ptr<QOffscreenSurface> surface(QRhiGles2InitParams::newFallbackSurface());
    QRhiGles2InitParams params;
    params.fallbackSurface = surface.get();
    std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::OpenGLES2, &params));
    if (!rhi) {
        result.error = QStringLiteral("QRhi OpenGL context creation failed");
        return result;
    }
    fillFromRhi(result, *rhi);
    result.adapterCount = 1;
    rhi.reset();  // the context must go before its fallback surface
    return result;
}

void hashFileStat(QCryptographicHash& hash, const QFileInfo& info)
{
    hash.addData(info.absoluteFilePath().toUtf8());
    hash.addData(QByteArray::number(info.size()));
    hash.addData(QByteArray::number(info.lastModified(QTimeZone::UTC).toMSecsSinceEpoch()));
}

}  // namespace

DevicePreference parseDevicePreference(const QString& text) noexcept
{
    if (text == u"integrated") {
        return DevicePreference::Integrated;
    }
    return text == u"discrete" ? DevicePreference::Discrete : DevicePreference::Auto;
}

int preferredDeviceIndex(const std::vector<DeviceKind>& devices, DevicePreference preference) noexcept
{
    if (preference == DevicePreference::Auto) {
        return -1;
    }
    const DeviceKind wanted =
        preference == DevicePreference::Integrated ? DeviceKind::Integrated : DeviceKind::Discrete;
    for (std::size_t i = 0; i < devices.size(); ++i) {
        if (devices[i] == wanted) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

ProbeResult probeRhi(Api api, DevicePreference preference)
{
    QElapsedTimer timer;
    timer.start();
    ProbeResult result = api == Api::Vulkan ? probeVulkan(preference) : probeOpenGl();
    qCInfo(lcGfx).noquote() << "probed" << apiName(api) << "in" << timer.elapsed()
                            << "ms:" << (result.ok ? result.deviceName : result.error);
    return result;
}

QString graphicsStackFingerprint()
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(qVersion());
    hash.addData(QGuiApplication::platformName().toUtf8());
    for (const char* variable :
         {"DRI_PRIME", "__NV_PRIME_RENDER_OFFLOAD", "__GLX_VENDOR_LIBRARY_NAME", "QT_VK_PHYSICAL_DEVICE_INDEX",
          "MESA_LOADER_DRIVER_OVERRIDE", "LIBGL_ALWAYS_SOFTWARE", "VK_ICD_FILENAMES", "VK_DRIVER_FILES"}) {
        hash.addData(variable);
        hash.addData(qgetenv(variable));
    }
    const QStringList icdDirs{QStringLiteral("/usr/share/vulkan/icd.d"), QStringLiteral("/etc/vulkan/icd.d")};
    for (const QString& dir : icdDirs) {
        for (const QFileInfo& info : QDir(dir).entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
            hashFileStat(hash, info);
        }
    }
    const QStringList libraries{
        QStringLiteral("/usr/lib/libGLX.so.0"),         QStringLiteral("/usr/lib/libEGL.so.1"),
        QStringLiteral("/usr/lib/libvulkan.so.1"),      QStringLiteral("/usr/lib/libgallium.so"),
        QStringLiteral("/usr/lib/libnvidia-glcore.so"), QStringLiteral("/proc/driver/nvidia/version")};
    for (const QString& library : libraries) {
        const QFileInfo info(library);
        if (info.exists()) {
            hashFileStat(hash, QFileInfo(info.canonicalFilePath()));
        }
    }
    // Driver version text changes even when mtimes do not (e.g. kernel module upgrades).
    if (auto nvidia = readWholeFile(QStringLiteral("/proc/driver/nvidia/version"))) {
        hash.addData(*nvidia);
    }
    return QString::fromLatin1(hash.result().toHex());
}

ProbeCache::ProbeCache(QString path, const QString& variant)
    : m_path(std::move(path))
    , m_fingerprint(variant.isEmpty() ? graphicsStackFingerprint() : graphicsStackFingerprint() + u'-' + variant)
{
    const auto content = readWholeFile(m_path);
    if (!content) {
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(*content).object();
    if (root.value(QStringLiteral("fingerprint")).toString() != m_fingerprint) {
        qCInfo(lcGfx) << "graphics stack changed; probe cache invalidated";
        return;
    }
    const QJsonObject results = root.value(QStringLiteral("results")).toObject();
    for (auto it = results.begin(); it != results.end(); ++it) {
        const ProbeResult result = ProbeResult::fromJson(it.value().toObject());
        m_results[result.api] = result;
    }
}

std::optional<ProbeResult> ProbeCache::lookup(Api api) const
{
    const auto it = m_results.find(api);
    if (it == m_results.end()) {
        return std::nullopt;
    }
    return it->second;
}

void ProbeCache::store(const ProbeResult& result)
{
    m_results[result.api] = result;
    m_dirty = true;
}

void ProbeCache::save() const
{
    if (!m_dirty) {
        return;
    }
    QJsonObject results;
    for (const auto& [api, result] : m_results) {
        results.insert(apiName(api), result.toJson());
    }
    const QJsonObject root{{QStringLiteral("fingerprint"), m_fingerprint}, {QStringLiteral("results"), results}};
    if (auto written = writeFileAtomic(m_path, QJsonDocument(root).toJson(QJsonDocument::Compact)); !written) {
        qCInfo(lcGfx).noquote() << "could not save probe cache:" << QString::fromStdString(written.error().describe());
    }
}

}  // namespace noctara::gfx
