#include "ui/SettingsModel.hpp"

#include "core/Strings.hpp"
#include "settings/Schema.hpp"
#include "settings/SettingsStore.hpp"

#include <QCoreApplication>

namespace noctara::ui {

namespace {

using settings::Definition;

QString translated(const char* source)
{
    return source == nullptr ? QString() : QCoreApplication::translate("Settings", source);
}

QString typeId(settings::Type type)
{
    switch (type) {
    case settings::Type::Bool: return QStringLiteral("bool");
    case settings::Type::Int: return QStringLiteral("int");
    case settings::Type::Real: return QStringLiteral("real");
    case settings::Type::String: return QStringLiteral("string");
    case settings::Type::Enum: return QStringLiteral("enum");
    case settings::Type::Color: return QStringLiteral("color");
    case settings::Type::Path: return QStringLiteral("path");
    case settings::Type::Url: return QStringLiteral("url");
    case settings::Type::StringList: return QStringLiteral("list");
    case settings::Type::Shortcut: return QStringLiteral("shortcut");
    }
    return {};
}

}  // namespace

SettingsModel::SettingsModel(settings::SettingsStore& store, const Availability& availability, QObject* parent)
    : QAbstractListModel(parent)
    , m_store(store)
    , m_availability(availability)
{
    connect(&m_store, &settings::SettingsStore::valueChanged, this, &SettingsModel::onValueChanged);
}

void SettingsModel::setRows(std::vector<std::size_t> rows)
{
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
    Q_EMIT countChanged();
}

void SettingsModel::setShowAdvanced(bool show)
{
    if (show != m_showAdvanced) {
        m_showAdvanced = show;
        refreshAll();
    }
}

void SettingsModel::refreshAll()
{
    if (!m_rows.empty()) {
        Q_EMIT dataChanged(index(0), index(count() - 1));
    }
}

void SettingsModel::onValueChanged(const QString& /*key*/)
{
    // A change can alter its own row and the visibility of dependent rows, so every row is told, but only about the
    // roles a value can change. Labels, options, reasons and statuses have their own full refresh (language,
    // availability, row status, advanced rows): re-reading them here rebuilt every option list and made the
    // segmented controls recreate their segments on each step of a slider.
    if (!m_rows.empty()) {
        Q_EMIT dataChanged(index(0), index(count() - 1),
                           {ValueRole, IsDefaultRole, OverriddenRole, OverrideHintRole, ShownRole, SectionStartRole});
    }
}

int SettingsModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : count();
}

bool SettingsModel::isShown(const Definition& def) const
{
    if (def.configOnly() || (def.advanced() && !m_showAdvanced)) {
        return false;
    }
    return conditionsHold(def);
}

// A row whose condition holds is still hidden when the row it depends on is hidden itself (e.g. the custom DNS
// servers list follows the server preset, which only exists in custom DNS mode).
bool SettingsModel::conditionsHold(const Definition& def) const
{
    if (!def.visibleWhen) {
        return true;
    }
    const QString current = m_store.value(def.visibleWhen->key).toString();
    const bool matches = std::ranges::any_of(def.visibleWhen->values, [&](std::string_view value) {
        return current == QLatin1StringView(value.data(), static_cast<qsizetype>(value.size()));
    });
    const Definition* parent = m_store.schema().find(def.visibleWhen->key);
    return matches && (parent == nullptr || conditionsHold(*parent));
}

QVariantList SettingsModel::options(const Definition& def) const
{
    QVariantList list;
    const auto disabled = m_availability.disabledOptions.value(latin(def.key));
    for (const settings::Option& option : def.options) {
        const QString id = latin(option.id);
        const char* reason = disabled.value(id, nullptr);
        list.append(QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("label"), option.translatable ? translated(option.label) : QString::fromUtf8(option.label)},
            {QStringLiteral("enabled"), reason == nullptr},
            {QStringLiteral("reason"), translated(reason)},
        });
    }
    return list;
}

QVariant SettingsModel::data(const QModelIndex& index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid)) {
        return {};
    }
    const std::size_t schemaIndex = m_rows.at(static_cast<std::size_t>(index.row()));
    const Definition& def = m_store.schema().at(schemaIndex);
    switch (static_cast<Role>(role)) {
    case KeyRole: return latin(def.key);
    case TypeRole: return typeId(def.type);
    case LabelRole: return translated(def.label);
    case DescriptionRole: return translated(def.description);
    case ValueRole: return m_store.value(schemaIndex);
    case DefaultValueRole: return def.defaultValue;
    // "Nothing to reset": a reset falls back to the system config's value, else the built-in default.
    case IsDefaultRole:
        return m_store.value(schemaIndex) ==
               m_store.layerValue(settings::Layer::System, schemaIndex).value_or(def.defaultValue);
    case OptionsRole: return options(def);
    case MinRole: return def.min;
    case MaxRole: return def.max;
    case StepRole: return def.step;
    case UnitRole: return translated(def.unit);
    case RequiresRestartRole: return def.requiresRestart();
    case AdvancedRole: return def.advanced();
    case OverriddenRole: return m_store.isOverridden(schemaIndex);
    case OverrideHintRole: {
        if (!m_store.isOverridden(schemaIndex)) {
            return QString();
        }
        if (m_store.effectiveLayer(schemaIndex) == settings::Layer::CommandLine) {
            return QCoreApplication::translate("Settings", "Set by a command-line flag for this session.");
        }
        //: %1 = environment variable name
        return QCoreApplication::translate("Settings", "Set by the environment variable %1.")
            .arg(settings::Schema::environmentVariable(def));
    }
    case ShownRole: return isShown(def);
    case DisabledReasonRole: return translated(m_availability.disabledSettings.value(latin(def.key), nullptr));
    case SectionRole: return latin(def.sectionId());
    case SectionLabelRole: {
        const settings::Section* section = m_store.schema().section(def.sectionId());
        return section != nullptr ? translated(section->label) : QString();
    }
    case SectionDescriptionRole: {
        const settings::Section* section = m_store.schema().section(def.sectionId());
        return section != nullptr ? translated(section->description) : QString();
    }
    case CategoryLabelRole: return translated(settings::categoryLabel(def.category));
    case StatusRole: return m_availability.statuses.value(latin(def.key));
    case SectionStartRole: {
        if (!isShown(def)) {
            return false;
        }
        for (int row = index.row() - 1; row >= 0; --row) {
            const Definition& previous = m_store.schema().at(m_rows.at(static_cast<std::size_t>(row)));
            if (previous.sectionId() != def.sectionId()) {
                break;
            }
            if (isShown(previous)) {
                return false;
            }
        }
        return true;
    }
    }
    return {};
}

QHash<int, QByteArray> SettingsModel::roleNames() const
{
    return {
        {KeyRole, "key"},
        {TypeRole, "type"},
        {LabelRole, "label"},
        {DescriptionRole, "description"},
        {ValueRole, "value"},
        {DefaultValueRole, "defaultValue"},
        {IsDefaultRole, "isDefault"},
        {OptionsRole, "options"},
        {MinRole, "minimum"},
        {MaxRole, "maximum"},
        {StepRole, "step"},
        {UnitRole, "unit"},
        {RequiresRestartRole, "requiresRestart"},
        {AdvancedRole, "advanced"},
        {OverriddenRole, "overridden"},
        {OverrideHintRole, "overrideHint"},
        {ShownRole, "shown"},
        {DisabledReasonRole, "disabledReason"},
        {SectionRole, "section"},
        {SectionLabelRole, "sectionLabel"},
        {SectionDescriptionRole, "sectionDescription"},
        {CategoryLabelRole, "categoryLabel"},
        {SectionStartRole, "sectionStart"},
        {StatusRole, "status"},
    };
}

}  // namespace noctara::ui
