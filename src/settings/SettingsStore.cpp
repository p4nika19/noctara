#include "settings/SettingsStore.hpp"

#include "core/Log.hpp"

#include <format>
#include <mutex>

namespace noctara::settings {

namespace {

constexpr std::size_t layerIndex(Layer layer) noexcept
{
    return static_cast<std::size_t>(layer);
}

QString keyString(std::string_view key)
{
    return QString::fromLatin1(key.data(), static_cast<qsizetype>(key.size()));
}

}  // namespace

std::string_view layerName(Layer layer) noexcept
{
    switch (layer) {
    case Layer::Default: return "default";
    case Layer::System: return "system config";
    case Layer::User: return "user config";
    case Layer::Environment: return "environment";
    case Layer::CommandLine: return "command line";
    }
    return "?";
}

SettingsStore::SettingsStore(const Schema& schema, QObject* parent)
    : QObject(parent)
    , m_schema(schema)
{
    for (auto& layer : m_layers) {
        layer.resize(schema.size());
    }
    auto& defaults = m_layers[layerIndex(Layer::Default)];
    for (std::size_t i = 0; i < schema.size(); ++i) {
        defaults[i] = schema.at(i).defaultValue;
    }
}

std::size_t SettingsStore::requireIndex(std::string_view key) const
{
    const auto index = m_schema.indexOf(key);
    Q_ASSERT_X(index.has_value(), "SettingsStore", "unknown settings key");
    if (!index) {
        qCCritical(lcSettings) << "unknown settings key" << keyString(key);
        return 0;
    }
    return *index;
}

QVariant SettingsStore::effectiveLocked(std::size_t index) const
{
    for (std::size_t layer = kLayerCount; layer-- > 0;) {
        if (const Slot& slot = m_layers[layer][index]; slot) {
            return *slot;
        }
    }
    return {};
}

QVariant SettingsStore::belowUserLocked(std::size_t index) const
{
    if (const Slot& system = m_layers[layerIndex(Layer::System)][index]; system) {
        return *system;
    }
    return m_layers[layerIndex(Layer::Default)][index].value_or(QVariant());
}

QVariant SettingsStore::value(std::string_view key) const
{
    return value(requireIndex(key));
}

QVariant SettingsStore::value(std::size_t index) const
{
    const std::shared_lock lock(m_mutex);
    return effectiveLocked(index);
}

Layer SettingsStore::effectiveLayer(std::size_t index) const
{
    const std::shared_lock lock(m_mutex);
    for (std::size_t layer = kLayerCount; layer-- > 0;) {
        if (m_layers[layer][index]) {
            return static_cast<Layer>(layer);
        }
    }
    return Layer::Default;
}

std::optional<QVariant> SettingsStore::layerValue(Layer layer, std::size_t index) const
{
    const std::shared_lock lock(m_mutex);
    return m_layers[layerIndex(layer)][index];
}

ValueMap SettingsStore::layerSnapshot(Layer layer) const
{
    const std::shared_lock lock(m_mutex);
    ValueMap out;
    const auto& slots = m_layers[layerIndex(layer)];
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (const Slot& slot = slots[i]; slot.has_value()) {
            out.emplace_back(i, slot.value());
        }
    }
    return out;
}

bool SettingsStore::isOverridden(std::size_t index) const
{
    const std::shared_lock lock(m_mutex);
    return m_layers[layerIndex(Layer::Environment)][index].has_value() ||
           m_layers[layerIndex(Layer::CommandLine)][index].has_value();
}

void SettingsStore::notifyChanges(const std::vector<QVariant>& before)
{
    for (std::size_t i = 0; i < before.size(); ++i) {
        const QVariant now = value(i);
        if (now != before[i]) {
            Q_EMIT valueChanged(keyString(m_schema.at(i).key), now);
        }
    }
}

void SettingsStore::replaceLayer(Layer layer, const ValueMap& values)
{
    Q_ASSERT(layer != Layer::Default);
    std::vector<QVariant> before(m_schema.size());
    {
        const std::unique_lock lock(m_mutex);
        for (std::size_t i = 0; i < m_schema.size(); ++i) {
            before[i] = effectiveLocked(i);
        }
        auto& slots = m_layers[layerIndex(layer)];
        std::ranges::fill(slots, std::nullopt);
        for (const auto& [index, val] : values) {
            if (index < slots.size()) {
                slots[index] = val;
            }
        }
    }
    notifyChanges(before);
}

Result<void> SettingsStore::setUserValue(std::string_view key, const QVariant& candidate)
{
    const auto index = m_schema.indexOf(key);
    if (!index) {
        return fail(ErrorCode::ConfigUnknownKey, std::string(key));
    }
    if (isOverridden(*index)) {
        return fail(ErrorCode::ConfigOverridden, std::format("{} is set by the environment or command line", key));
    }
    auto normalised = validate(m_schema.at(*index), candidate);
    if (!normalised) {
        return std::unexpected(std::move(normalised.error()));
    }

    QVariant before;
    {
        const std::unique_lock lock(m_mutex);
        before = effectiveLocked(*index);
        // Values equal to what lower layers provide are not stored: the config file stays minimal.
        auto& slot = m_layers[layerIndex(Layer::User)][*index];
        if (*normalised == belowUserLocked(*index)) {
            slot.reset();
        } else {
            slot = *normalised;
        }
    }
    if (value(*index) != before) {
        Q_EMIT valueChanged(keyString(key), value(*index));
    }
    Q_EMIT userLayerEdited();
    return {};
}

void SettingsStore::eraseUserValues(const std::vector<std::size_t>& indices)
{
    std::vector<QVariant> before(m_schema.size());
    bool erased = false;
    {
        const std::unique_lock lock(m_mutex);
        for (std::size_t i = 0; i < m_schema.size(); ++i) {
            before[i] = effectiveLocked(i);
        }
        auto& slots = m_layers[layerIndex(Layer::User)];
        for (const std::size_t index : indices) {
            erased = erased || slots[index].has_value();
            slots[index].reset();
        }
    }
    notifyChanges(before);
    if (erased) {
        Q_EMIT userLayerEdited();
    }
}

void SettingsStore::resetUserValue(std::string_view key)
{
    eraseUserValues({requireIndex(key)});
}

void SettingsStore::resetSection(std::string_view sectionId)
{
    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < m_schema.size(); ++i) {
        if (m_schema.at(i).sectionId() == sectionId) {
            indices.push_back(i);
        }
    }
    eraseUserValues(indices);
}

void SettingsStore::resetCategory(Category category)
{
    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < m_schema.size(); ++i) {
        if (m_schema.at(i).category == category) {
            indices.push_back(i);
        }
    }
    eraseUserValues(indices);
}

void SettingsStore::resetAll()
{
    std::vector<std::size_t> indices(m_schema.size());
    for (std::size_t i = 0; i < indices.size(); ++i) {
        indices[i] = i;
    }
    eraseUserValues(indices);
}

}  // namespace noctara::settings
