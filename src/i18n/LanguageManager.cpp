#include "i18n/LanguageManager.hpp"

#include "core/Log.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"

#include <QCoreApplication>
#include <QLibraryInfo>

namespace noctara::i18n {

namespace {

constexpr LanguageInfo kLanguages[] = {
    {"en", "English"},
    {"ru", "Русский"},
};

}  // namespace

std::span<const LanguageInfo> availableLanguages() noexcept
{
    return kLanguages;
}

QString matchLanguage(const QStringList& uiLanguages)
{
    for (const QString& tag : uiLanguages) {
        const QString code = QLocale(tag).name().section(u'_', 0, 0);
        for (const LanguageInfo& info : kLanguages) {
            if (code == QLatin1StringView(info.code)) {
                return code;
            }
        }
    }
    return QStringLiteral("en");
}

LanguageManager::LanguageManager(settings::SettingsStore& settings, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
{
    connect(&m_settings, &settings::SettingsStore::valueChanged, this, [this](const QString& key) {
        if (key == QLatin1StringView(settings::keys::Language.data(),
                                     static_cast<qsizetype>(settings::keys::Language.size()))) {
            apply();
        }
    });
    apply();
}

LanguageManager::~LanguageManager()
{
    removeTranslators();
}

void LanguageManager::removeTranslators()
{
    for (auto* translator : {m_appTranslator.get(), m_qtTranslator.get()}) {
        if (translator != nullptr) {
            QCoreApplication::removeTranslator(translator);
        }
    }
    m_appTranslator.reset();
    m_qtTranslator.reset();
}

void LanguageManager::apply()
{
    const QString setting = m_settings.stringValue(settings::keys::Language);
    const QLocale system = QLocale::system();
    const QString language = setting == u"system" ? matchLanguage(system.uiLanguages()) : setting;
    if (language == m_language && m_appTranslator) {
        return;
    }

    removeTranslators();
    // Keep the user's regional formats when the chosen language matches the system language.
    m_locale = system.name().section(u'_', 0, 0) == language ? system : QLocale(language);
    QLocale::setDefault(m_locale);

    // English is loaded too: it carries the plural forms of numerus strings.
    auto app = std::make_unique<QTranslator>();
    if (app->load(QStringLiteral(":/i18n/noctara_%1.qm").arg(language))) {
        QCoreApplication::installTranslator(app.get());
        m_appTranslator = std::move(app);
    } else {
        qCWarning(lcI18n) << "missing translation catalogue for" << language;
    }
    auto qt = std::make_unique<QTranslator>();
    if (language != u"en" && qt->load(m_locale, QStringLiteral("qtbase"), QStringLiteral("_"),
                                      QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        QCoreApplication::installTranslator(qt.get());
        m_qtTranslator = std::move(qt);
    }

    m_language = language;
    qCInfo(lcI18n) << "language" << language << "locale" << m_locale.name();
    Q_EMIT languageChanged();
}

}  // namespace noctara::i18n
