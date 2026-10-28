#include "TestUtil.hpp"

#include "auth/BrowserCookieSource.hpp"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <array>
#include <memory>
#include <openssl/evp.h>
#include <optional>
#include <sqlite3.h>
#include <string>

using namespace noctara;

namespace {

struct DatabaseCloser {
    void operator()(sqlite3* database) const { sqlite3_close(database); }
};
using Database = std::unique_ptr<sqlite3, DatabaseCloser>;

Database openDatabase(const QString& path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    sqlite3* database = nullptr;
    REQUIRE(sqlite3_open(QFile::encodeName(path).constData(), &database) == SQLITE_OK);
    return Database(database);
}

void execute(sqlite3* database, const char* sql)
{
    REQUIRE(sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
}

QByteArray encryptV10(const QByteArray& token, const QByteArray& password)
{
    std::array<unsigned char, 16> key{};
    constexpr std::array<unsigned char, 9> salt{'s', 'a', 'l', 't', 'y', 's', 'a', 'l', 't'};
    // Chromium on Linux uses a single PBKDF2 iteration; match it so this fixture mirrors a real browser.
    REQUIRE(PKCS5_PBKDF2_HMAC(password.constData(), static_cast<int>(password.size()), salt.data(),
                              static_cast<int>(salt.size()), 1, EVP_sha1(), static_cast<int>(key.size()),
                              key.data()) == 1);
    EVP_CIPHER_CTX* raw = EVP_CIPHER_CTX_new();
    REQUIRE(raw != nullptr);
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context(raw, EVP_CIPHER_CTX_free);
    std::array<unsigned char, 16> iv{};
    iv.fill(' ');
    QByteArray encrypted(token.size() + EVP_MAX_BLOCK_LENGTH, Qt::Uninitialized);
    int first = 0;
    int final = 0;
    REQUIRE(EVP_EncryptInit_ex(context.get(), EVP_aes_128_cbc(), nullptr, key.data(), iv.data()) == 1);
    REQUIRE(EVP_EncryptUpdate(context.get(), reinterpret_cast<unsigned char*>(encrypted.data()), &first,
                              reinterpret_cast<const unsigned char*>(token.constData()),
                              static_cast<int>(token.size())) == 1);
    REQUIRE(EVP_EncryptFinal_ex(context.get(), reinterpret_cast<unsigned char*>(encrypted.data()) + first, &final) ==
            1);
    encrypted.truncate(first + final);
    return QByteArrayLiteral("v10") + encrypted;
}

void insertChromiumCookie(sqlite3* database, const QByteArray& host, const QByteArray& name,
                          const QByteArray& encrypted)
{
    sqlite3_stmt* statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(database,
                               "INSERT INTO cookies(host_key,name,encrypted_value,expires_utc,last_access_utc) "
                               "VALUES(?,?,?,0,100)",
                               -1, &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_bind_text(statement, 1, host.constData(), static_cast<int>(host.size()), SQLITE_STATIC) ==
            SQLITE_OK);
    REQUIRE(sqlite3_bind_text(statement, 2, name.constData(), static_cast<int>(name.size()), SQLITE_STATIC) ==
            SQLITE_OK);
    REQUIRE(sqlite3_bind_blob(statement, 3, encrypted.constData(), static_cast<int>(encrypted.size()), SQLITE_STATIC) ==
            SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_DONE);
    REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
}

QString makeChromiumProfile(const QString& root, const QByteArray& token, const QByteArray& password,
                            const QByteArray& host = "soundcloud.com")
{
    const QString databasePath = root + QStringLiteral("/Default/Network/Cookies");
    auto database = openDatabase(databasePath);
    execute(database.get(), "CREATE TABLE cookies(host_key TEXT,name TEXT,encrypted_value BLOB,expires_utc INTEGER,"
                            "last_access_utc INTEGER)");
    insertChromiumCookie(database.get(), host, QByteArrayLiteral("oauth_token"), encryptV10(token, password));
    database.reset();

    QFile localState(root + QStringLiteral("/Local State"));
    REQUIRE(localState.open(QIODevice::WriteOnly));
    const QJsonObject profile{{QStringLiteral("name"), QStringLiteral("Personal")}};
    const QJsonObject cache{{QStringLiteral("Default"), profile}};
    const QJsonObject profileObject{{QStringLiteral("info_cache"), cache}};
    const QJsonObject top{{QStringLiteral("profile"), profileObject}};
    REQUIRE(localState.write(QJsonDocument(top).toJson()) > 0);
    return databasePath;
}

QString makeFirefoxProfile(const QString& root, const QByteArray& token, const QByteArray& host = "soundcloud.com")
{
    const QString databasePath = root + QStringLiteral("/default-profile/cookies.sqlite");
    auto database = openDatabase(databasePath);
    execute(database.get(), "CREATE TABLE moz_cookies(host TEXT,name TEXT,value TEXT,expiry INTEGER,"
                            "lastAccessed INTEGER)");
    sqlite3_stmt* statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(database.get(),
                               "INSERT INTO moz_cookies(host,name,value,expiry,lastAccessed) VALUES(?,?,?,0,100)", -1,
                               &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_bind_text(statement, 1, host.constData(), static_cast<int>(host.size()), SQLITE_STATIC) ==
            SQLITE_OK);
    REQUIRE(sqlite3_bind_text(statement, 2, "oauth_token", -1, SQLITE_STATIC) == SQLITE_OK);
    REQUIRE(sqlite3_bind_text(statement, 3, token.constData(), static_cast<int>(token.size()), SQLITE_STATIC) ==
            SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_DONE);
    REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
    database.reset();

    QFile settings(root + QStringLiteral("/profiles.ini"));
    REQUIRE(settings.open(QIODevice::WriteOnly));
    REQUIRE(settings.write("[Profile0]\nName=Default\nIsRelative=1\nPath=default-profile\n") > 0);
    return databasePath;
}

Result<QVector<auth::BrowserProfile>> discover(auth::BrowserCookieSource& source)
{
    QObject context;
    auto result = std::make_shared<std::optional<Result<QVector<auth::BrowserProfile>>>>();
    source.discover(&context,
                    [result](Result<QVector<auth::BrowserProfile>> profiles) { result->emplace(std::move(profiles)); });
    REQUIRE(test::waitUntil([&] { return result->has_value(); }));
    return std::move(**result);
}

Result<QByteArray> importToken(auth::BrowserCookieSource& source, const auth::BrowserProfile& profile)
{
    QObject context;
    auto result = std::make_shared<std::optional<Result<QByteArray>>>();
    source.importToken(profile, &context, [result](Result<QByteArray> token) { result->emplace(std::move(token)); });
    REQUIRE(test::waitUntil([&] { return result->has_value(); }));
    return std::move(**result);
}

QByteArray fileContents(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

}  // namespace

TEST_SUITE("auth.browser-cookies")
{
    TEST_CASE("discovers Chromium metadata without decrypting and imports only after selection")
    {
        QTemporaryDir home;
        const QString root = home.path() + QStringLiteral("/.config/chromium");
        const QString database =
            makeChromiumProfile(root, QByteArrayLiteral("browser-token"), QByteArrayLiteral("fixture-password"));
        const QByteArray before = fileContents(database);
        int passwordCalls = 0;
        auth::BrowserCookieSource source(home.path(), [&](const auth::BrowserProfile&) -> Result<QByteArray> {
            ++passwordCalls;
            return QByteArrayLiteral("fixture-password");
        });

        auto profiles = discover(source);
        REQUIRE(profiles);
        REQUIRE(profiles->size() == 1);
        CHECK(profiles->constFirst().browserName == u"Chromium");
        CHECK(profiles->constFirst().profileName == u"Personal");
        CHECK(passwordCalls == 0);

        auto token = importToken(source, profiles->constFirst());
        REQUIRE(token);
        CHECK(*token == QByteArrayLiteral("browser-token"));
        CHECK(passwordCalls == 1);
        CHECK(fileContents(database) == before);
    }

    TEST_CASE("discovers Flatpak and Snap profile layouts")
    {
        QTemporaryDir home;
        makeChromiumProfile(home.path() + QStringLiteral("/.var/app/org.chromium.Chromium/config/chromium"),
                            QByteArrayLiteral("flatpak-token"), QByteArrayLiteral("password"));
        makeChromiumProfile(home.path() + QStringLiteral("/snap/chromium/common/chromium"),
                            QByteArrayLiteral("snap-token"), QByteArrayLiteral("password"));
        auth::BrowserCookieSource source(
            home.path(), [](const auth::BrowserProfile&) { return Result<QByteArray>(QByteArrayLiteral("password")); });
        auto profiles = discover(source);
        REQUIRE(profiles);
        CHECK(profiles->size() == 2);
        CHECK(profiles->at(0).databasePath.contains(QStringLiteral("/.var/app/")));
        CHECK(profiles->at(1).databasePath.contains(QStringLiteral("/snap/")));
    }

    TEST_CASE("imports Firefox cookies from profiles.ini")
    {
        QTemporaryDir home;
        makeFirefoxProfile(home.path() + QStringLiteral("/.mozilla/firefox"), QByteArrayLiteral("firefox-token"));
        auth::BrowserCookieSource source(home.path(), {});
        auto profiles = discover(source);
        REQUIRE(profiles);
        REQUIRE(profiles->size() == 1);
        CHECK(profiles->constFirst().family == auth::BrowserFamily::Firefox);
        auto token = importToken(source, profiles->constFirst());
        REQUIRE(token);
        CHECK(*token == QByteArrayLiteral("firefox-token"));
    }

    TEST_CASE("imports Firefox from the XDG mozilla profile layout")
    {
        QTemporaryDir home;
        makeFirefoxProfile(home.path() + QStringLiteral("/.config/mozilla/firefox"),
                           QByteArrayLiteral("xdg-firefox-token"));
        auth::BrowserCookieSource source(home.path(), {});
        auto profiles = discover(source);
        REQUIRE(profiles);
        REQUIRE(profiles->size() == 1);
        CHECK(profiles->constFirst().databasePath.contains(QStringLiteral("/.config/mozilla/firefox/")));
        auto token = importToken(source, profiles->constFirst());
        REQUIRE(token);
        CHECK(*token == QByteArrayLiteral("xdg-firefox-token"));
    }

    // Opt-in because it reads the current user's real browser session; never prints cookie values.
    TEST_CASE("real Firefox profile import" * doctest::skip())
    {
        auth::BrowserCookieSource source;
        auto profiles = discover(source);
        REQUIRE(profiles);
        bool imported = false;
        for (const auto& profile : *profiles) {
            if (profile.family != auth::BrowserFamily::Firefox) {
                continue;
            }
            auto token = importToken(source, profile);
            if (token) {
                imported = !token->isEmpty();
                break;
            }
        }
        CHECK(imported);
    }

    TEST_CASE("rejects unrelated hosts, unsupported encryption and wrong Safe Storage passwords")
    {
        QTemporaryDir home;
        const QString root = home.path() + QStringLiteral("/.config/chromium");
        makeChromiumProfile(root, QByteArrayLiteral("not-soundcloud"), QByteArrayLiteral("password"),
                            QByteArrayLiteral("example.com"));
        auth::BrowserCookieSource source(
            home.path(), [](const auth::BrowserProfile&) { return Result<QByteArray>(QByteArrayLiteral("password")); });
        auto profiles = discover(source);
        REQUIRE(profiles);
        CHECK(test::errorOf(importToken(source, profiles->constFirst())) == ErrorCode::NotFound);

        auto database = openDatabase(root + QStringLiteral("/Default/Network/Cookies"));
        insertChromiumCookie(database.get(), QByteArrayLiteral("soundcloud.com"), QByteArrayLiteral("oauth_token"),
                             QByteArrayLiteral("v11-not-supported"));
        database.reset();
        CHECK(test::errorOf(importToken(source, profiles->constFirst())) == ErrorCode::Unsupported);

        auto other = openDatabase(root + QStringLiteral("/Default/Network/Cookies"));
        execute(other.get(), "DELETE FROM cookies");
        insertChromiumCookie(other.get(), QByteArrayLiteral("soundcloud.com"), QByteArrayLiteral("oauth_token"),
                             encryptV10(QByteArrayLiteral("secret"), QByteArrayLiteral("correct")));
        other.reset();
        CHECK(test::errorOf(importToken(source, profiles->constFirst())) == ErrorCode::PermissionDenied);
    }

    TEST_CASE("reads a committed WAL without modifying the live database")
    {
        QTemporaryDir home;
        const QString root = home.path() + QStringLiteral("/.config/chromium");
        const QString path = root + QStringLiteral("/Default/Network/Cookies");
        auto database = openDatabase(path);
        execute(database.get(), "PRAGMA journal_mode=WAL");
        execute(database.get(), "CREATE TABLE cookies(host_key TEXT,name TEXT,encrypted_value BLOB,expires_utc INTEGER,"
                                "last_access_utc INTEGER)");
        insertChromiumCookie(database.get(), QByteArrayLiteral("soundcloud.com"), QByteArrayLiteral("oauth_token"),
                             encryptV10(QByteArrayLiteral("wal-token"), QByteArrayLiteral("password")));
        QFileInfo wal(path + QStringLiteral("-wal"));
        REQUIRE(wal.exists());
        const QByteArray walBefore = fileContents(wal.absoluteFilePath());

        auth::BrowserCookieSource source(
            home.path(), [](const auth::BrowserProfile&) { return Result<QByteArray>(QByteArrayLiteral("password")); });
        auth::BrowserProfile profile{QStringLiteral("test"),
                                     QStringLiteral("Chromium"),
                                     QStringLiteral("Default"),
                                     auth::BrowserFamily::Chromium,
                                     path,
                                     {}};
        auto token = importToken(source, profile);
        REQUIRE(token);
        CHECK(*token == QByteArrayLiteral("wal-token"));
        CHECK(fileContents(wal.absoluteFilePath()) == walBefore);
    }
}
