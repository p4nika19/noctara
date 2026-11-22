#include "ui/ThemesController.hpp"

#include "i18n/ErrorMessages.hpp"
#include "i18n/LanguageManager.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"
#include "theme/Theme.hpp"
#include "theme/ThemeLibrary.hpp"

#include <QDesktopServices>

#include <algorithm>

namespace noctara::ui {

namespace {

QString issueMessage(const theme::ThemeIssue& issue)
{
    using Kind = theme::ThemeIssue::Kind;
    switch (issue.kind) {
    case Kind::Syntax:
        return ThemesController::tr("Line %1: the file is not valid TOML, so none of it applies.").arg(issue.line);
    case Kind::UnknownToken:
        return ThemesController::tr("Line %1: “%2” is not a theme token and was skipped.")
            .arg(QString::number(issue.line), issue.key);
    case Kind::InvalidValue:
        return ThemesController::tr("Line %1: “%2” has an invalid value and was skipped.")
            .arg(QString::number(issue.line), issue.key);
    }
    return {};
}

}  // namespace

ThemesController::ThemesController(theme::ThemeLibrary& library, settings::SettingsStore& settings,
                                   i18n::LanguageManager& languages, const theme::Theme* theme, QObject* parent)
    : QObject(parent)
    , m_library(library)
    , m_settings(settings)
    , m_theme(theme)
{
    connect(&m_library, &theme::ThemeLibrary::themesChanged, this, &ThemesController::themesChanged);
    connect(&m_library, &theme::ThemeLibrary::activeChanged, this, &ThemesController::themesChanged);
    connect(&languages, &i18n::LanguageManager::languageChanged, this, &ThemesController::themesChanged);
    connect(&m_settings, &settings::SettingsStore::valueChanged, this, [this](const QString& key) {
        if (key == QLatin1StringView(settings::keys::CustomTheme) ||
            (m_theme != nullptr && m_theme->themeValue(key.toStdString()).isValid())) {
            Q_EMIT themesChanged();
        }
    });
    if (m_theme != nullptr) {
        connect(m_theme, &theme::Theme::userThemeChanged, this, &ThemesController::themesChanged);
    }
}

QVariantList ThemesController::themes() const
{
    QVariantList out;
    for (const theme::ThemeEntry& entry : m_library.themes()) {
        QStringList issues;
        for (const theme::ThemeIssue& issue : entry.issues) {
            issues.append(issueMessage(issue));
        }
        out.append(QVariantMap{{QStringLiteral("id"), entry.id},
                               {QStringLiteral("name"), entry.name},
                               {QStringLiteral("description"), entry.description},
                               {QStringLiteral("author"), entry.author},
                               {QStringLiteral("usable"), entry.theme != nullptr},
                               {QStringLiteral("issues"), issues}});
    }
    return out;
}

QString ThemesController::selected() const
{
    return m_library.selectedId();
}

bool ThemesController::customized() const
{
    if (m_theme == nullptr) {
        return false;
    }
    const auto& definitions = m_settings.schema().definitions();
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        if (m_theme->themeValue(definitions[i].key).isValid() &&
            m_settings.effectiveLayer(i) != settings::Layer::Default) {
            return true;
        }
    }
    return false;
}

QStringList ThemesController::selectedIssues() const
{
    QStringList out;
    if (selected().isEmpty() || !m_library.scanned()) {
        return out;
    }
    const theme::ThemeEntry* entry = m_library.selectedEntry();
    if (entry == nullptr) {
        out.append(tr("The theme “%1” is not in the themes folder; the built-in look is used.").arg(selected()));
        return out;
    }
    for (const theme::ThemeIssue& issue : entry->issues) {
        out.append(issueMessage(issue));
    }
    return out;
}

void ThemesController::select(const QString& id)
{
    if (const Result<void> set = m_settings.setUserValue(settings::keys::CustomTheme, id); !set) {
        Q_EMIT notice(QStringLiteral("warning"), i18n::errorMessage(set.error().code));
    }
}

void ThemesController::importFile(const QUrl& file)
{
    m_library.importFile(file.toLocalFile(), this, [this](Result<QString> id) {
        if (!id) {
            Q_EMIT notice(QStringLiteral("error"), id.error().code == ErrorCode::ConfigSyntaxError
                                                       ? tr("That file is not a valid theme (TOML syntax error).")
                                                       : i18n::errorMessage(id.error().code));
            return;
        }
        if (const Result<void> set = m_settings.setUserValue(settings::keys::CustomTheme, *id); !set) {
            //: %1 = why the theme could not be selected, a full sentence
            Q_EMIT notice(QStringLiteral("warning"),
                          tr("Theme imported but not applied. %1").arg(i18n::errorMessage(set.error().code)));
            return;
        }
        Q_EMIT notice(QStringLiteral("info"), tr("Theme imported and applied."));
    });
}

void ThemesController::exportTheme(const QString& id, const QUrl& file)
{
    m_library.exportTheme(id, file.toLocalFile(), this, [this](Result<void> done) {
        if (!done) {
            Q_EMIT notice(QStringLiteral("error"), i18n::errorMessage(done.error().code));
            return;
        }
        Q_EMIT notice(QStringLiteral("info"), tr("Theme exported."));
    });
}

void ThemesController::openFolder()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_library.directory()));
}

}  // namespace noctara::ui
