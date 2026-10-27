#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>

#include <vector>

namespace noctara::settings {
class SettingsStore;
struct Definition;
}  // namespace noctara::settings

namespace noctara::ui {

// Runtime availability of settings or individual enum options (e.g. blur styles in software mode).
// Reasons are translation sources in the "Settings" context, translated when displayed.
struct Availability {
    QHash<QString, const char*> disabledSettings;                 // key -> reason
    QHash<QString, QHash<QString, const char*>> disabledOptions;  // key -> option id -> reason
};

// Rows of the generated settings UI. One model per page (lazily created) plus one for search results.
// Labels are translated at data() time, so a language switch only needs a dataChanged.
class SettingsModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        KeyRole = Qt::UserRole + 1,
        TypeRole,
        LabelRole,
        DescriptionRole,
        ValueRole,
        DefaultValueRole,
        IsDefaultRole,
        OptionsRole,
        MinRole,
        MaxRole,
        StepRole,
        UnitRole,
        RequiresRestartRole,
        AdvancedRole,
        OverriddenRole,
        OverrideHintRole,
        ShownRole,
        DisabledReasonRole,
        SectionRole,
        SectionLabelRole,
        SectionDescriptionRole,
        CategoryLabelRole,
        SectionStartRole,  // first shown row of its section: the delegate draws the section header
    };
    Q_ENUM(Role)

    SettingsModel(settings::SettingsStore& store, const Availability& availability, QObject* parent = nullptr);

    void setRows(std::vector<std::size_t> rows);
    void setShowAdvanced(bool show);
    void refreshAll();  // language switch, availability or visibility changes

    [[nodiscard]] int count() const { return static_cast<int>(m_rows.size()); }
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

Q_SIGNALS:
    void countChanged();

private:
    [[nodiscard]] bool isShown(const settings::Definition& def) const;
    [[nodiscard]] QVariantList options(const settings::Definition& def) const;
    void onValueChanged(const QString& key);

    settings::SettingsStore& m_store;
    const Availability& m_availability;
    std::vector<std::size_t> m_rows;
    bool m_showAdvanced = false;
};

}  // namespace noctara::ui
