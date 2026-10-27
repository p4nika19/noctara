#include "i18n/Formatter.hpp"

#include "i18n/LanguageManager.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"

#include <QCoreApplication>

#include <array>
#include <cmath>
#include <cstdlib>

namespace noctara::i18n {

namespace {

QString translateFormat(const char* text)
{
    return QCoreApplication::translate("Format", text);
}

QString twoDigits(qint64 value)
{
    return QStringLiteral("%1").arg(value, 2, 10, QChar(u'0'));
}

// One decimal, dropped when it is zero ("1.2" / "12"), with the locale's decimal separator.
QString compactMantissa(const QLocale& locale, double value)
{
    const double rounded = std::round(value * 10.0) / 10.0;
    if (rounded >= 100.0 || std::fmod(rounded, 1.0) == 0.0) {
        return locale.toString(static_cast<qint64>(std::llround(rounded)));
    }
    return locale.toString(rounded, 'f', 1);
}

}  // namespace

Formatter::Formatter(settings::SettingsStore& settings, LanguageManager& languages, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_languages(languages)
{
    connect(&m_languages, &LanguageManager::languageChanged, this, [this] {
        ++m_revision;
        Q_EMIT changed();
    });
    connect(&m_settings, &settings::SettingsStore::valueChanged, this, [this](const QString& key) {
        if (key.startsWith(u"general.formatting.")) {
            ++m_revision;
            Q_EMIT changed();
        }
    });
}

QString Formatter::duration(qint64 milliseconds) const
{
    if (milliseconds < 0) {
        return QStringLiteral("–:––");
    }
    const qint64 totalSeconds = milliseconds / 1000;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds / 60) % 60;
    const qint64 seconds = totalSeconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3").arg(hours).arg(twoDigits(minutes), twoDigits(seconds));
    }
    return QStringLiteral("%1:%2").arg(minutes).arg(twoDigits(seconds));
}

QString Formatter::number(qint64 value) const
{
    return m_languages.locale().toString(value);
}

QString Formatter::count(qint64 value) const
{
    const QLocale& locale = m_languages.locale();
    const qint64 magnitude = std::llabs(value);
    if (!m_settings.boolValue(settings::keys::CompactNumbers) || magnitude < 1000) {
        return locale.toString(value);
    }
    const auto v = static_cast<double>(value);
    if (magnitude < 999'950) {
        return QCoreApplication::translate("Format", "%1K", "compact thousands, e.g. 1.2K")
            .arg(compactMantissa(locale, v / 1e3));
    }
    if (magnitude < 999'950'000) {
        return QCoreApplication::translate("Format", "%1M", "compact millions, e.g. 3.4M")
            .arg(compactMantissa(locale, v / 1e6));
    }
    return QCoreApplication::translate("Format", "%1B", "compact billions, e.g. 1.1B")
        .arg(compactMantissa(locale, v / 1e9));
}

QString Formatter::bytes(qint64 value) const
{
    static constexpr std::array<const char*, 5> units = {
        QT_TRANSLATE_NOOP("Format", "%1 B"), QT_TRANSLATE_NOOP("Format", "%1 KB"), QT_TRANSLATE_NOOP("Format", "%1 MB"),
        QT_TRANSLATE_NOOP("Format", "%1 GB"), QT_TRANSLATE_NOOP("Format", "%1 TB")};
    double size = static_cast<double>(std::max<qint64>(0, value));
    std::size_t unit = 0;
    while (size >= 1024.0 && unit + 1 < units.size()) {
        size /= 1024.0;
        ++unit;
    }
    const QLocale& locale = m_languages.locale();
    const QString amount = unit == 0 || size >= 100.0 ? locale.toString(static_cast<qint64>(std::llround(size)))
                                                      : locale.toString(size, 'f', 1);
    return translateFormat(units.at(unit)).arg(amount);
}

QString Formatter::relativeTime(const QDateTime& when) const
{
    return relativeTime(when, QDateTime::currentDateTimeUtc());
}

QString Formatter::relativeTime(const QDateTime& when, const QDateTime& now) const
{
    if (!when.isValid()) {
        return {};
    }
    const qint64 seconds = when.secsTo(now);
    if (seconds < 60) {
        return QCoreApplication::translate("Format", "just now");
    }
    const auto n = [](qint64 value) {
        return static_cast<int>(std::min<qint64>(value, std::numeric_limits<int>::max()));
    };
    constexpr qint64 kHour = 3600;
    constexpr qint64 kDay = 24 * kHour;
    constexpr qint64 kMonth = 30 * kDay;
    constexpr qint64 kYear = 365 * kDay;
    if (seconds < kHour) {
        return QCoreApplication::translate("Format", "%n minute(s) ago", nullptr, n(seconds / 60));
    }
    if (seconds < kDay) {
        return QCoreApplication::translate("Format", "%n hour(s) ago", nullptr, n(seconds / kHour));
    }
    if (seconds < kMonth) {
        return QCoreApplication::translate("Format", "%n day(s) ago", nullptr, n(seconds / kDay));
    }
    if (seconds < kYear) {
        return QCoreApplication::translate("Format", "%n month(s) ago", nullptr, n(seconds / kMonth));
    }
    return QCoreApplication::translate("Format", "%n year(s) ago", nullptr, n(seconds / kYear));
}

bool Formatter::use24Hour() const
{
    const QString mode = m_settings.stringValue(settings::keys::TimeFormat);
    if (mode == u"24h") {
        return true;
    }
    if (mode == u"12h") {
        return false;
    }
    return !m_languages.locale().timeFormat(QLocale::ShortFormat).contains(u'A', Qt::CaseInsensitive);
}

QString Formatter::time(const QDateTime& when) const
{
    const QDateTime local = when.toLocalTime();
    return m_languages.locale().toString(local.time(),
                                         use24Hour() ? QStringLiteral("HH:mm") : QStringLiteral("h:mm AP"));
}

QString Formatter::date(const QDateTime& when) const
{
    return m_languages.locale().toString(when.toLocalTime().date(), QLocale::ShortFormat);
}

QString Formatter::tracks(int count)
{
    return QCoreApplication::translate("Format", "%n track(s)", nullptr, count);
}

}  // namespace noctara::i18n
