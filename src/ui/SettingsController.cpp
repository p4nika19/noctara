#include "ui/SettingsController.hpp"

#include "i18n/ErrorMessages.hpp"
#include "i18n/LanguageManager.hpp"
#include "settings/ConfigFile.hpp"
#include "settings/Schema.hpp"
#include "settings/SettingsStore.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QQmlEngine>
#include <QUrl>

namespace noctara::ui {

namespace {

using settings::Category;

QString latin(std::string_view text)
{
    return QString::fromLatin1(text.data(), static_cast<qsizetype>(text.size()));
}

const char* categoryIcon(Category category)
{
    switch (category) {
    case Category::General: return "sliders-horizontal";
    case Category::Appearance: return "palette";
    case Category::Layout: return "layout-grid";
    case Category::Playback: return "play";
    case Category::Library: return "library";
    case Category::Keyboard: return "keyboard";
    case Category::Performance: return "cpu";
    case Category::Network: return "globe";
    case Category::Storage: return "hard-drive";
    case Category::Accounts: return "user";
    case Category::Advanced: return "settings";
    case Category::About: return "info";
    }
    return "settings";
}

std::optional<Category> categoryFromId(const QString& id)
{
    for (int c = 0; c < settings::kCategoryCount; ++c) {
        const auto category = static_cast<Category>(c);
        if (latin(settings::categoryId(category)) == id) {
            return category;
        }
    }
    return std::nullopt;
}

}  // namespace

SettingsController::SettingsController(settings::SettingsStore& store, settings::ConfigFile& config,
                                       i18n::LanguageManager& languages, QObject* parent)
    : QObject(parent)
    , m_store(store)
    , m_config(config)
    , m_search(std::make_unique<SettingsModel>(store, m_availability))
{
    QQmlEngine::setObjectOwnership(m_search.get(), QQmlEngine::CppOwnership);
    for (std::size_t i = 0; i < store.schema().size(); ++i) {
        if (store.schema().at(i).requiresRestart()) {
            m_launchValues.emplace_back(i, store.value(i));
        }
    }
    connect(&m_store, &settings::SettingsStore::valueChanged, this, &SettingsController::updateRestartPending);
    connect(&languages, &i18n::LanguageManager::languageChanged, this, [this] {
        refreshModels();
        rebuildSearch();
        Q_EMIT categoriesChanged();
    });
}

SettingsController::~SettingsController() = default;

QVariantList SettingsController::categories() const
{
    QVariantList list;
    for (int c = 0; c < settings::kCategoryCount; ++c) {
        const auto category = static_cast<Category>(c);
        const bool hasRows = std::ranges::any_of(m_store.schema().definitions(), [&](const settings::Definition& def) {
            return def.category == category && !def.configOnly();
        });
        if (!hasRows && category != Category::About) {
            continue;
        }
        list.append(QVariantMap{
            {QStringLiteral("id"), latin(settings::categoryId(category))},
            {QStringLiteral("label"), QCoreApplication::translate("Settings", settings::categoryLabel(category))},
            {QStringLiteral("icon"), QString::fromLatin1(categoryIcon(category))},
        });
    }
    return list;
}

QString SettingsController::configPath() const
{
    return m_config.path();
}

void SettingsController::setShowAdvanced(bool show)
{
    if (show == m_showAdvanced) {
        return;
    }
    m_showAdvanced = show;
    for (auto& [id, model] : m_pages) {
        model->setShowAdvanced(show);
    }
    m_search->setShowAdvanced(show);
    rebuildSearch();
    Q_EMIT showAdvancedChanged();
}

SettingsModel* SettingsController::page(const QString& categoryId)
{
    if (const auto it = m_pages.find(categoryId); it != m_pages.end()) {
        return it->second.get();
    }
    const auto category = categoryFromId(categoryId);
    if (!category) {
        return nullptr;
    }
    std::vector<std::size_t> rows;
    for (std::size_t i = 0; i < m_store.schema().size(); ++i) {
        const auto& def = m_store.schema().at(i);
        if (def.category == *category && !def.configOnly()) {
            rows.push_back(i);
        }
    }
    auto model = std::make_unique<SettingsModel>(m_store, m_availability);
    // Returned to QML from an invokable: without this the JS garbage collector would take ownership.
    QQmlEngine::setObjectOwnership(model.get(), QQmlEngine::CppOwnership);
    model->setShowAdvanced(m_showAdvanced);
    model->setRows(std::move(rows));
    SettingsModel* raw = model.get();
    m_pages.emplace(categoryId, std::move(model));
    return raw;
}

void SettingsController::setSearchText(const QString& text)
{
    if (text == m_searchText) {
        return;
    }
    m_searchText = text;
    rebuildSearch();
    Q_EMIT searchTextChanged();
}

void SettingsController::rebuildSearch()
{
    const QString needle = m_searchText.trimmed();
    std::vector<std::size_t> rows;
    if (!needle.isEmpty()) {
        const auto matches = [&needle](const QString& haystack) {
            return haystack.contains(needle, Qt::CaseInsensitive);
        };
        for (std::size_t i = 0; i < m_store.schema().size(); ++i) {
            const auto& def = m_store.schema().at(i);
            if (def.configOnly() || (def.advanced() && !m_showAdvanced)) {
                continue;
            }
            const auto* section = m_store.schema().section(def.sectionId());
            if (matches(QCoreApplication::translate("Settings", def.label)) ||
                matches(QCoreApplication::translate("Settings", def.description)) ||
                (section != nullptr && matches(QCoreApplication::translate("Settings", section->label))) ||
                matches(latin(def.key))) {
                rows.push_back(i);
            }
        }
    }
    m_search->setRows(std::move(rows));
}

QString SettingsController::setValue(const QString& key, const QVariant& value)
{
    const auto* def = m_store.schema().find(key.toStdString());
    if (def == nullptr) {
        return i18n::errorMessage(ErrorCode::ConfigUnknownKey);
    }
    // QML numbers arrive as double; integer settings need integral values.
    QVariant candidate = value;
    if (def->type == settings::Type::Int && candidate.canConvert<double>()) {
        candidate = QVariant::fromValue(static_cast<qint64>(std::llround(candidate.toDouble())));
    } else if (def->type == settings::Type::Real && candidate.canConvert<double>()) {
        candidate = QVariant::fromValue(candidate.toDouble());
    } else if (def->type == settings::Type::StringList) {
        candidate = QVariant::fromValue(candidate.toStringList());
    }
    if (auto result = m_store.setUserValue(def->key, candidate); !result) {
        return i18n::errorMessage(result.error().code);
    }
    return {};
}

QVariant SettingsController::value(const QString& key) const
{
    const auto* def = m_store.schema().find(key.toStdString());
    return def != nullptr ? m_store.value(def->key) : QVariant();
}

void SettingsController::reset(const QString& key)
{
    if (const auto* def = m_store.schema().find(key.toStdString())) {
        m_store.resetUserValue(def->key);
    }
}

void SettingsController::resetSection(const QString& sectionId)
{
    m_store.resetSection(sectionId.toStdString());
}

void SettingsController::resetCategory(const QString& categoryId)
{
    if (const auto category = categoryFromId(categoryId)) {
        m_store.resetCategory(*category);
    }
}

void SettingsController::openConfigFile() const
{
    m_config.flush();
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_config.path()));
}

QString SettingsController::exportProfile(const QUrl& target) const
{
    if (auto result = m_config.exportTo(target.toLocalFile()); !result) {
        return i18n::errorMessage(result.error().code);
    }
    return {};
}

QString SettingsController::importProfile(const QUrl& source)
{
    auto result = m_config.importFrom(source.toLocalFile());
    if (!result) {
        return i18n::errorMessage(result.error().code);
    }
    return {};
}

void SettingsController::setAvailability(Availability availability)
{
    m_availability = std::move(availability);
    refreshModels();
}

void SettingsController::refreshModels()
{
    for (auto& [id, model] : m_pages) {
        model->refreshAll();
    }
    m_search->refreshAll();
}

void SettingsController::updateRestartPending()
{
    const bool pending = std::ranges::any_of(
        m_launchValues, [this](const auto& entry) { return m_store.value(entry.first) != entry.second; });
    if (pending != m_restartPending) {
        m_restartPending = pending;
        Q_EMIT restartPendingChanged();
    }
}

}  // namespace noctara::ui
