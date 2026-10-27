#include "gfx/GraphicsPolicy.hpp"

#include "core/Log.hpp"

#include <QJsonArray>
#include <QJsonDocument>

namespace noctara::gfx {

namespace {

GraphicsDecision software(Reason reason, bool notice, QString detail, bool safeMode = false)
{
    GraphicsDecision decision;
    decision.hardware = false;
    decision.safeMode = safeMode;
    decision.reason = reason;
    decision.showNotice = notice;
    decision.liquidGlassAllowed = false;
    decision.detail = std::move(detail);
    return decision;
}

std::optional<BlockAction> parseAction(const QString& text)
{
    if (text == u"software") {
        return BlockAction::Software;
    }
    if (text == u"avoid-api") {
        return BlockAction::AvoidApi;
    }
    if (text == u"no-liquid-glass") {
        return BlockAction::NoLiquidGlass;
    }
    return std::nullopt;
}

QRegularExpression pattern(const QJsonObject& json, const char* key)
{
    return QRegularExpression(json.value(QLatin1StringView(key)).toString(), QRegularExpression::CaseInsensitiveOption);
}

}  // namespace

Acceleration parseAcceleration(const QString& text) noexcept
{
    if (text == u"on") {
        return Acceleration::On;
    }
    if (text == u"off") {
        return Acceleration::Off;
    }
    return Acceleration::Auto;
}

Api parseApi(const QString& text) noexcept
{
    if (text == u"vulkan") {
        return Api::Vulkan;
    }
    if (text == u"opengl") {
        return Api::OpenGL;
    }
    return Api::Auto;
}

QString apiName(Api api)
{
    switch (api) {
    case Api::Auto: return QStringLiteral("auto");
    case Api::OpenGL: return QStringLiteral("opengl");
    case Api::Vulkan: return QStringLiteral("vulkan");
    }
    return {};
}

QString deviceKindName(DeviceKind kind)
{
    switch (kind) {
    case DeviceKind::Unknown: return QStringLiteral("unknown");
    case DeviceKind::Integrated: return QStringLiteral("integrated");
    case DeviceKind::Discrete: return QStringLiteral("discrete");
    case DeviceKind::Virtual: return QStringLiteral("virtual");
    case DeviceKind::Cpu: return QStringLiteral("cpu");
    }
    return {};
}

QJsonObject ProbeResult::toJson() const
{
    return QJsonObject{{QStringLiteral("ok"), ok},
                       {QStringLiteral("api"), apiName(api)},
                       {QStringLiteral("device"), deviceName},
                       {QStringLiteral("driver"), driver},
                       {QStringLiteral("vendorId"), static_cast<qint64>(vendorId)},
                       {QStringLiteral("deviceId"), static_cast<qint64>(deviceId)},
                       {QStringLiteral("kind"), static_cast<int>(kind)},
                       {QStringLiteral("adapters"), adapterCount},
                       {QStringLiteral("error"), error}};
}

ProbeResult ProbeResult::fromJson(const QJsonObject& json)
{
    ProbeResult result;
    result.ok = json.value(QStringLiteral("ok")).toBool();
    result.api = parseApi(json.value(QStringLiteral("api")).toString());
    result.deviceName = json.value(QStringLiteral("device")).toString();
    result.driver = json.value(QStringLiteral("driver")).toString();
    result.vendorId = static_cast<std::uint64_t>(json.value(QStringLiteral("vendorId")).toInteger());
    result.deviceId = static_cast<std::uint64_t>(json.value(QStringLiteral("deviceId")).toInteger());
    const int kind = json.value(QStringLiteral("kind")).toInt();
    result.kind =
        kind >= 0 && kind <= static_cast<int>(DeviceKind::Cpu) ? static_cast<DeviceKind>(kind) : DeviceKind::Unknown;
    result.adapterCount = json.value(QStringLiteral("adapters")).toInt();
    result.error = json.value(QStringLiteral("error")).toString();
    return result;
}

bool isSoftwareRasterizer(const QString& deviceName)
{
    static const QRegularExpression names(
        QStringLiteral("llvmpipe|softpipe|lavapipe|swiftshader|swrast|microsoft basic render"),
        QRegularExpression::CaseInsensitiveOption);
    return names.match(deviceName).hasMatch();
}

Blocklist parseBlocklist(const QByteArray& json)
{
    Blocklist list;
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError) {
        qCWarning(lcGfx) << "gpu blocklist is not valid JSON:" << error.errorString();
        return list;
    }
    for (const auto& value : document.object().value(QStringLiteral("entries")).toArray()) {
        const QJsonObject object = value.toObject();
        const auto action = parseAction(object.value(QStringLiteral("action")).toString());
        BlocklistEntry entry;
        entry.id = object.value(QStringLiteral("id")).toString();
        entry.deviceName = pattern(object, "deviceName");
        entry.driver = pattern(object, "driver");
        entry.api = parseApi(object.value(QStringLiteral("api")).toString());
        entry.reason = object.value(QStringLiteral("reason")).toString();
        if (object.contains(QStringLiteral("vendorId"))) {
            entry.vendorId = static_cast<std::uint64_t>(object.value(QStringLiteral("vendorId")).toInteger());
        }
        if (!action || entry.id.isEmpty() || !entry.deviceName.isValid() || !entry.driver.isValid()) {
            qCWarning(lcGfx) << "skipping invalid gpu blocklist entry" << entry.id;
            continue;
        }
        entry.action = *action;
        list.push_back(std::move(entry));
    }
    return list;
}

const BlocklistEntry* matchBlocklist(const Blocklist& list, const ProbeResult& probe)
{
    for (const BlocklistEntry& entry : list) {
        if (entry.api != Api::Auto && entry.api != probe.api) {
            continue;
        }
        if (entry.vendorId && *entry.vendorId != probe.vendorId) {
            continue;
        }
        if (!entry.deviceName.pattern().isEmpty() && !entry.deviceName.match(probe.deviceName).hasMatch()) {
            continue;
        }
        if (!entry.driver.pattern().isEmpty() && !entry.driver.match(probe.driver).hasMatch()) {
            continue;
        }
        return &entry;
    }
    return nullptr;
}

std::vector<Api> candidateApis(Api requested)
{
    // OpenGL is Qt Quick's default and best-tested backend on Linux; Vulkan is the alternative.
    if (requested == Api::Vulkan) {
        return {Api::Vulkan, Api::OpenGL};
    }
    return {Api::OpenGL, Api::Vulkan};
}

GraphicsDecision decide(const GraphicsRequest& request, const ProbeFunction& probe, const Blocklist& blocklist)
{
    if (request.safeModeFlag) {
        return software(Reason::SafeModeFlag, true, QStringLiteral("--safe-mode"), true);
    }
    if (request.crashSentinelFound) {
        return software(Reason::CrashRecovery, true, QStringLiteral("previous launch crashed during graphics init"),
                        true);
    }
    if (request.acceleration == Acceleration::Off) {
        return software(Reason::UserDisabled, false, QStringLiteral("hardware acceleration disabled"));
    }

    const bool forced = request.acceleration == Acceleration::On;
    std::optional<ProbeResult> lastFailure;
    std::optional<GraphicsDecision> rejected;
    for (const Api api : candidateApis(request.api)) {
        ProbeResult result = probe(api);
        result.api = api;
        if (!result.ok) {
            qCInfo(lcGfx).noquote() << "graphics probe failed for" << apiName(api) << ':' << result.error;
            lastFailure = result;
            continue;
        }

        bool liquidGlass = true;
        if (const BlocklistEntry* entry = matchBlocklist(blocklist, result)) {
            qCInfo(lcGfx).noquote() << "gpu blocklist match" << entry->id << ':' << entry->reason;
            if (entry->action == BlockAction::AvoidApi) {
                lastFailure = result;
                continue;
            }
            if (entry->action == BlockAction::Software && !forced) {
                rejected = software(Reason::Blocklisted, true, entry->reason);
                rejected->probe = result;
                break;
            }
            liquidGlass = entry->action != BlockAction::NoLiquidGlass;
        }

        const bool cpuDevice = result.kind == DeviceKind::Cpu || isSoftwareRasterizer(result.deviceName);
        if (cpuDevice && !forced) {
            rejected = software(Reason::SoftwareRasterizer, true, result.deviceName);
            rejected->probe = result;
            break;
        }

        GraphicsDecision decision;
        decision.hardware = true;
        decision.api = api;
        decision.liquidGlassAllowed = liquidGlass && !cpuDevice;
        decision.reason = (request.api != Api::Auto && api != request.api) ? Reason::ApiFallback : Reason::Hardware;
        decision.showNotice = decision.reason == Reason::ApiFallback;
        decision.detail = result.deviceName;
        decision.probe = std::move(result);
        return decision;
    }
    if (rejected) {
        return *rejected;
    }
    GraphicsDecision failed = software(Reason::ProbeFailed, true,
                                       lastFailure ? lastFailure->error : QStringLiteral("no graphics API available"));
    failed.probe = std::move(lastFailure);
    return failed;
}

}  // namespace noctara::gfx
