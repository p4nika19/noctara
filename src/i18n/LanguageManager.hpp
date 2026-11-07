#pragma once

#include <QLocale>
#include <QObject>
#include <QString>
#include <QTranslator>

#include <memory>
#include <span>

namespace noctara::settings {
class SettingsStore;
}

namespace noctara::i18n {

struct LanguageInfo {
    const char* code;        // "en", "ru": also the .qm suffix (:/i18n/noctara_<code>.qm)
    const char* nativeName;  // endonym, UTF-8, never translated
};

// Picks the first supported language from a list of UI languages (e.g. QLocale::uiLanguages()).
[[nodiscard]] QString matchLanguage(const QStringList& uiLanguages);

// Installs translators for the configured language and switches them live. Listeners (the QML engine's
// retranslate(), tray, MPRIS) react to languageChanged().
class LanguageManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString language READ language NOTIFY languageChanged)
    Q_PROPERTY(QString localeName READ localeName NOTIFY languageChanged)

public:
    explicit LanguageManager(settings::SettingsStore& settings, QObject* parent = nullptr);
    ~LanguageManager() override;

    LanguageManager(const LanguageManager&) = delete;
    LanguageManager& operator=(const LanguageManager&) = delete;

    [[nodiscard]] const QString& language() const noexcept { return m_language; }
    [[nodiscard]] const QLocale& locale() const noexcept { return m_locale; }
    [[nodiscard]] QString localeName() const { return m_locale.name(); }

Q_SIGNALS:
    void languageChanged();

private:
    void apply();
    void removeTranslators();

    settings::SettingsStore& m_settings;
    QString m_language;
    QLocale m_locale;
    std::unique_ptr<QTranslator> m_appTranslator;
    std::unique_ptr<QTranslator> m_qtTranslator;
};

}  // namespace noctara::i18n
