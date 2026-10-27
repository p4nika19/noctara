#include "TestUtil.hpp"

#include "i18n/ErrorMessages.hpp"
#include "i18n/Formatter.hpp"
#include "i18n/LanguageManager.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"

#include <QSignalSpy>

using namespace noctara;
namespace keys = noctara::settings::keys;

namespace {

struct Fixture {
    settings::SettingsStore store{settings::Schema::builtin()};
    std::unique_ptr<i18n::LanguageManager> languages;
    std::unique_ptr<i18n::Formatter> format;

    explicit Fixture(const char* language)
    {
        REQUIRE(store.setUserValue(keys::Language, QString::fromLatin1(language)).has_value());
        languages = std::make_unique<i18n::LanguageManager>(store);
        format = std::make_unique<i18n::Formatter>(store, *languages);
    }
};

}  // namespace

TEST_SUITE("i18n")
{
    TEST_CASE("Russian plural categories (one / few / many)")
    {
        Fixture f("ru");
        const std::pair<int, const char*> expected[] = {
            {0, "0 треков"}, {1, "1 трек"},    {2, "2 трека"},    {5, "5 треков"},   {11, "11 треков"},
            {21, "21 трек"}, {22, "22 трека"}, {25, "25 треков"}, {101, "101 трек"},
        };
        for (const auto& [n, text] : expected) {
            INFO("n = ", n);
            CHECK(i18n::Formatter::tracks(n) == QString::fromUtf8(text));
        }
    }

    TEST_CASE("English plurals and live switching")
    {
        Fixture f("en");
        CHECK(i18n::Formatter::tracks(1) == QStringLiteral("1 track"));
        CHECK(i18n::Formatter::tracks(2) == QStringLiteral("2 tracks"));

        QSignalSpy changed(f.languages.get(), &i18n::LanguageManager::languageChanged);
        REQUIRE(f.store.setUserValue(keys::Language, QStringLiteral("ru")).has_value());
        CHECK(changed.count() == 1);
        CHECK(f.languages->language() == QStringLiteral("ru"));
        CHECK(i18n::errorMessage(ErrorCode::Offline) == QString::fromUtf8("Нет подключения к сети."));
        REQUIRE(f.store.setUserValue(keys::Language, QStringLiteral("en")).has_value());
        CHECK(i18n::errorMessage(ErrorCode::Offline) == QStringLiteral("You are offline."));
    }

    TEST_CASE("relative times use plural forms")
    {
        Fixture f("ru");
        const QDateTime now = QDateTime::fromString(QStringLiteral("2026-10-27T12:00:00Z"), Qt::ISODate);
        CHECK(f.format->relativeTime(now.addSecs(-30), now) == QString::fromUtf8("только что"));
        CHECK(f.format->relativeTime(now.addDays(-3), now) == QString::fromUtf8("3 дня назад"));
        CHECK(f.format->relativeTime(now.addDays(-21), now) == QString::fromUtf8("21 день назад"));
        CHECK(f.format->relativeTime(now.addSecs(-5 * 3600), now) == QString::fromUtf8("5 часов назад"));
    }

    TEST_CASE("compact numbers, sizes and durations follow the locale")
    {
        Fixture en("en");
        CHECK(en.format->count(999) == QStringLiteral("999"));
        CHECK(en.format->count(1234) == QStringLiteral("1.2K"));
        CHECK(en.format->count(12'000) == QStringLiteral("12K"));
        CHECK(en.format->count(3'400'000) == QStringLiteral("3.4M"));
        CHECK(en.format->bytes(1536) == QStringLiteral("1.5 KB"));
        CHECK(en.format->duration(185'000) == QStringLiteral("3:05"));
        CHECK(en.format->duration(3'723'000) == QStringLiteral("1:02:03"));

        REQUIRE(en.store.setUserValue(keys::CompactNumbers, false).has_value());
        CHECK(en.format->count(1234) == QStringLiteral("1,234"));

        Fixture ru("ru");
        CHECK(ru.format->count(1234) == QString::fromUtf8("1,2 тыс."));
        CHECK(ru.format->count(3'400'000) == QString::fromUtf8("3,4 млн"));
        CHECK(ru.format->bytes(1536) == QString::fromUtf8("1,5 КБ"));
    }

    TEST_CASE("language matching falls back to English")
    {
        CHECK(i18n::matchLanguage({QStringLiteral("ru-RU"), QStringLiteral("en-US")}) == QStringLiteral("ru"));
        CHECK(i18n::matchLanguage({QStringLiteral("de-DE")}) == QStringLiteral("en"));
        CHECK(i18n::matchLanguage({}) == QStringLiteral("en"));
    }

    TEST_CASE("every error code has a translated Russian message")
    {
        Fixture f("ru");
        for (int code = 0; code <= static_cast<int>(ErrorCode::Forbidden); ++code) {
            const auto error = static_cast<ErrorCode>(code);
            const QString russian = i18n::errorMessage(error);
            INFO(std::string(errorCodeName(error)));
            CHECK_FALSE(russian.isEmpty());
            CHECK(russian.contains(QRegularExpression(QStringLiteral("[а-яА-ЯёЁ]"))));
        }
    }
}
