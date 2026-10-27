#pragma once

#include "ui/SettingsModel.hpp"

#include <QObject>
#include <QVariant>

#include <array>
#include <map>
#include <memory>

namespace noctara::settings {
class ConfigFile;
class SettingsStore;
}  // namespace noctara::settings

namespace noctara::i18n {
class LanguageManager;
}

namespace noctara::ui {

// QML "SettingsUi" singleton: pages generated from the schema, search, edits, resets, profile
// import/export and restart tracking for settings that only apply on the next launch.
class SettingsController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList categories READ categories NOTIFY categoriesChanged)
    Q_PROPERTY(bool showAdvanced READ showAdvanced WRITE setShowAdvanced NOTIFY showAdvancedChanged)
    Q_PROPERTY(bool restartPending READ restartPending NOTIFY restartPendingChanged)
    Q_PROPERTY(QString configPath READ configPath CONSTANT)
    Q_PROPERTY(noctara::ui::SettingsModel* searchResults READ searchResults CONSTANT)
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)

public:
    SettingsController(settings::SettingsStore& store, settings::ConfigFile& config, i18n::LanguageManager& languages,
                       QObject* parent = nullptr);
    ~SettingsController() override;

    [[nodiscard]] QVariantList categories() const;
    [[nodiscard]] bool showAdvanced() const noexcept { return m_showAdvanced; }
    void setShowAdvanced(bool show);
    [[nodiscard]] bool restartPending() const noexcept { return m_restartPending; }
    [[nodiscard]] QString configPath() const;
    [[nodiscard]] SettingsModel* searchResults() const noexcept { return m_search.get(); }
    [[nodiscard]] const QString& searchText() const noexcept { return m_searchText; }
    void setSearchText(const QString& text);

    // Lazily creates the page model for a category id ("appearance", "performance", ...).
    Q_INVOKABLE noctara::ui::SettingsModel* page(const QString& categoryId);

    // Returns an empty string on success, otherwise a translated error message.
    Q_INVOKABLE QString setValue(const QString& key, const QVariant& value);
    Q_INVOKABLE QVariant value(const QString& key) const;
    Q_INVOKABLE void reset(const QString& key);
    Q_INVOKABLE void resetSection(const QString& sectionId);
    Q_INVOKABLE void resetCategory(const QString& categoryId);
    Q_INVOKABLE void openConfigFile() const;
    Q_INVOKABLE QString exportProfile(const QUrl& target) const;
    Q_INVOKABLE QString importProfile(const QUrl& source);

    void setAvailability(Availability availability);

Q_SIGNALS:
    void categoriesChanged();
    void showAdvancedChanged();
    void restartPendingChanged();
    void searchTextChanged();

private:
    void refreshModels();
    void updateRestartPending();
    void rebuildSearch();

    settings::SettingsStore& m_store;
    settings::ConfigFile& m_config;
    Availability m_availability;
    std::map<QString, std::unique_ptr<SettingsModel>> m_pages;
    std::unique_ptr<SettingsModel> m_search;
    QString m_searchText;
    bool m_showAdvanced = false;
    bool m_restartPending = false;
    std::vector<std::pair<std::size_t, QVariant>> m_launchValues;  // restart-required settings at startup
};

}  // namespace noctara::ui
