#pragma once

#include "core/Error.hpp"
#include "settings/Schema.hpp"

#include <QObject>
#include <QStringList>
#include <QVariant>

#include <array>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <vector>

namespace noctara::settings {

// Precedence, lowest first.
enum class Layer : std::uint8_t { Default, System, User, Environment, CommandLine };
inline constexpr std::size_t kLayerCount = 5;

[[nodiscard]] std::string_view layerName(Layer layer) noexcept;

// Typed, observable, layered settings. Reads are thread-safe; mutations and change notifications
// happen on the thread that owns the store (the UI thread).
class SettingsStore : public QObject {
    Q_OBJECT

public:
    explicit SettingsStore(const Schema& schema, QObject* parent = nullptr);

    [[nodiscard]] const Schema& schema() const noexcept { return m_schema; }

    [[nodiscard]] QVariant value(std::string_view key) const;
    [[nodiscard]] QVariant value(std::size_t index) const;
    [[nodiscard]] bool boolValue(std::string_view key) const { return value(key).toBool(); }
    [[nodiscard]] qint64 intValue(std::string_view key) const { return value(key).toLongLong(); }
    [[nodiscard]] double realValue(std::string_view key) const { return value(key).toDouble(); }
    [[nodiscard]] QString stringValue(std::string_view key) const { return value(key).toString(); }
    [[nodiscard]] QStringList listValue(std::string_view key) const { return value(key).toStringList(); }

    [[nodiscard]] Layer effectiveLayer(std::size_t index) const;
    [[nodiscard]] std::optional<QVariant> layerValue(Layer layer, std::size_t index) const;
    [[nodiscard]] ValueMap layerSnapshot(Layer layer) const;
    // True when an environment variable or CLI flag pins this setting (UI edits are refused).
    [[nodiscard]] bool isOverridden(std::size_t index) const;

    // Replaces an entire layer (config file, environment, CLI). Values must already be validated.
    void replaceLayer(Layer layer, const ValueMap& values);

    // Interactive edits from the UI. Validate, write to the user layer and request persistence.
    [[nodiscard]] Result<void> setUserValue(std::string_view key, const QVariant& candidate);
    void resetUserValue(std::string_view key);
    void resetSection(std::string_view sectionId);
    void resetCategory(Category category);
    void resetAll();

Q_SIGNALS:
    void valueChanged(const QString& key, const QVariant& value);
    // The user layer changed through the API above (not through a file reload): persist it.
    void userLayerEdited();

private:
    using Slot = std::optional<QVariant>;

    [[nodiscard]] QVariant effectiveLocked(std::size_t index) const;
    [[nodiscard]] QVariant belowUserLocked(std::size_t index) const;
    [[nodiscard]] std::size_t requireIndex(std::string_view key) const;
    void eraseUserValues(const std::vector<std::size_t>& indices);
    void notifyChanges(const std::vector<QVariant>& before);

    const Schema& m_schema;
    mutable std::shared_mutex m_mutex;
    std::array<std::vector<Slot>, kLayerCount> m_layers;
};

}  // namespace noctara::settings
