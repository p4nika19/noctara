#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>

namespace noctara::settings {
class SettingsStore;
}

namespace noctara::i18n {

class LanguageManager;

// Locale-aware formatting for QML ("Format" singleton) and C++. Durations stay tabular and stable;
// sizes, compact numbers and relative times are translated with correct plural forms.
class Formatter : public QObject {
    Q_OBJECT
    // Bumped on language/format changes so QML bindings that call the invokables re-evaluate.
    Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
    Formatter(settings::SettingsStore& settings, LanguageManager& languages, QObject* parent = nullptr);

    [[nodiscard]] int revision() const noexcept { return m_revision; }

    // 3:05, 1:02:03. Negative/unknown durations render as "–:––".
    Q_INVOKABLE [[nodiscard]] QString duration(qint64 milliseconds) const;
    // 1.2K / 1,2 тыс. (or the full locale number when compact numbers are off).
    Q_INVOKABLE [[nodiscard]] QString count(qint64 value) const;
    Q_INVOKABLE [[nodiscard]] QString number(qint64 value) const;
    // 512 KB, 1.4 GB (binary multiples, translated units).
    Q_INVOKABLE [[nodiscard]] QString bytes(qint64 value) const;
    // "3 days ago" / "3 дня назад"; "just now" under a minute.
    Q_INVOKABLE [[nodiscard]] QString relativeTime(const QDateTime& when) const;
    [[nodiscard]] QString relativeTime(const QDateTime& when, const QDateTime& now) const;
    Q_INVOKABLE [[nodiscard]] QString time(const QDateTime& when) const;
    Q_INVOKABLE [[nodiscard]] QString date(const QDateTime& when) const;

    // Plural helpers used by C++ code (tray, notifications).
    [[nodiscard]] static QString tracks(int count);

Q_SIGNALS:
    void changed();

private:
    [[nodiscard]] bool use24Hour() const;

    settings::SettingsStore& m_settings;
    LanguageManager& m_languages;
    int m_revision = 0;
};

}  // namespace noctara::i18n
