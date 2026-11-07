#include "TestUtil.hpp"

#include "auth/BrowserCookieSource.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <memory>
#include <openssl/evp.h>
#include <optional>
#include <sqlite3.h>
#include <string>

using namespace noctara;

// Fixture cookies name only the fields that matter to a test; the rest keep their defaults.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

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

// What Chromium's Linux os_crypt writes: PBKDF2-HMAC-SHA1("saltysalt", 1 iteration) → AES-128-CBC, IV of spaces,
// prefixed with the version. From database version 24 on, the plaintext starts with SHA-256(host_key).
QByteArray encryptChromium(const QByteArray& token, const QByteArray& password, const QByteArray& prefix = "v10",
                           const std::optional<QByteArray>& hashedHost = std::nullopt)
{
    std::array<unsigned char, 16> key{};
    constexpr std::array<unsigned char, 9> salt{'s', 'a', 'l', 't', 'y', 's', 'a', 'l', 't'};
    REQUIRE(PKCS5_PBKDF2_HMAC(password.constData(), static_cast<int>(password.size()), salt.data(),
                              static_cast<int>(salt.size()), 1, EVP_sha1(), static_cast<int>(key.size()),
                              key.data()) == 1);
    const QByteArray plaintext =
        hashedHost ? QCryptographicHash::hash(*hashedHost, QCryptographicHash::Sha256) + token : token;
    EVP_CIPHER_CTX* raw = EVP_CIPHER_CTX_new();
    REQUIRE(raw != nullptr);
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context(raw, EVP_CIPHER_CTX_free);
    std::array<unsigned char, 16> iv{};
    iv.fill(' ');
    QByteArray encrypted(plaintext.size() + EVP_MAX_BLOCK_LENGTH, Qt::Uninitialized);
    int first = 0;
    int final = 0;
    REQUIRE(EVP_EncryptInit_ex(context.get(), EVP_aes_128_cbc(), nullptr, key.data(), iv.data()) == 1);
    REQUIRE(EVP_EncryptUpdate(context.get(), reinterpret_cast<unsigned char*>(encrypted.data()), &first,
                              reinterpret_cast<const unsigned char*>(plaintext.constData()),
                              static_cast<int>(plaintext.size())) == 1);
    REQUIRE(EVP_EncryptFinal_ex(context.get(), reinterpret_cast<unsigned char*>(encrypted.data()) + first, &final) ==
            1);
    encrypted.truncate(first + final);
    return prefix + encrypted;
}

// The columns of a current Chromium cookie database (sqlite_persistent_cookie_store.cc, version 24).
constexpr auto kChromiumSchema =
    "CREATE TABLE meta(key LONGVARCHAR NOT NULL UNIQUE PRIMARY KEY, value LONGVARCHAR);"
    "CREATE TABLE cookies(creation_utc INTEGER NOT NULL DEFAULT 0, host_key TEXT NOT NULL,"
    " top_frame_site_key TEXT NOT NULL DEFAULT '', name TEXT NOT NULL, value TEXT NOT NULL DEFAULT '',"
    " encrypted_value BLOB NOT NULL DEFAULT '', path TEXT NOT NULL DEFAULT '/', expires_utc INTEGER NOT NULL,"
    " is_secure INTEGER NOT NULL DEFAULT 1, is_httponly INTEGER NOT NULL DEFAULT 0,"
    " last_access_utc INTEGER NOT NULL, has_expires INTEGER NOT NULL DEFAULT 1,"
    " is_persistent INTEGER NOT NULL DEFAULT 1, priority INTEGER NOT NULL DEFAULT 1,"
    " samesite INTEGER NOT NULL DEFAULT -1, source_scheme INTEGER NOT NULL DEFAULT 2,"
    " source_port INTEGER NOT NULL DEFAULT 443, last_update_utc INTEGER NOT NULL DEFAULT 0,"
    " source_type INTEGER NOT NULL DEFAULT 0, has_cross_site_ancestor INTEGER NOT NULL DEFAULT 0);";

void setChromiumVersion(sqlite3* database, int version)
{
    const std::string sql =
        "INSERT OR REPLACE INTO meta(key,value) VALUES('version','" + std::to_string(version) + "')";
    execute(database, sql.c_str());
}

struct ChromiumCookie {
    QByteArray host = "soundcloud.com";
    QByteArray name = "oauth_token";
    QByteArray encrypted;
    QByteArray plain;
    qint64 expires = 0;
    qint64 lastAccess = 100;
};

void insertChromiumCookie(sqlite3* database, const ChromiumCookie& cookie)
{
    sqlite3_stmt* statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(database,
                               "INSERT INTO cookies(host_key,name,value,encrypted_value,expires_utc,last_access_utc) "
                               "VALUES(?,?,?,?,?,?)",
                               -1, &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_bind_text(statement, 1, cookie.host.constData(), static_cast<int>(cookie.host.size()),
                              SQLITE_STATIC) == SQLITE_OK);
    REQUIRE(sqlite3_bind_text(statement, 2, cookie.name.constData(), static_cast<int>(cookie.name.size()),
                              SQLITE_STATIC) == SQLITE_OK);
    REQUIRE(sqlite3_bind_text(statement, 3, cookie.plain.constData(), static_cast<int>(cookie.plain.size()),
                              SQLITE_STATIC) == SQLITE_OK);
    REQUIRE(sqlite3_bind_blob(statement, 4, cookie.encrypted.constData(), static_cast<int>(cookie.encrypted.size()),
                              SQLITE_STATIC) == SQLITE_OK);
    REQUIRE(sqlite3_bind_int64(statement, 5, cookie.expires) == SQLITE_OK);
    REQUIRE(sqlite3_bind_int64(statement, 6, cookie.lastAccess) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_DONE);
    REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
}

void writeLocalState(const QString& root)
{
    QFile localState(root + QStringLiteral("/Local State"));
    REQUIRE(localState.open(QIODevice::WriteOnly));
    const QJsonObject profile{{QStringLiteral("name"), QStringLiteral("Personal")}};
    const QJsonObject cache{{QStringLiteral("Default"), profile}};
    const QJsonObject profileObject{{QStringLiteral("info_cache"), cache}};
    const QJsonObject top{{QStringLiteral("profile"), profileObject}};
    REQUIRE(localState.write(QJsonDocument(top).toJson()) > 0);
}

// An empty Chromium profile (schema version `version`) under `root`/Default; returns the cookie database path.
QString makeChromiumProfile(const QString& root, int version = 24)
{
    const QString databasePath = root + QStringLiteral("/Default/Network/Cookies");
    auto database = openDatabase(databasePath);
    execute(database.get(), kChromiumSchema);
    setChromiumVersion(database.get(), version);
    database.reset();
    writeLocalState(root);
    return databasePath;
}

QString makeChromiumProfile(const QString& root, const ChromiumCookie& cookie, int version = 24)
{
    const QString databasePath = makeChromiumProfile(root, version);
    auto database = openDatabase(databasePath);
    insertChromiumCookie(database.get(), cookie);
    return databasePath;
}

void replaceChromiumCookies(const QString& databasePath, const ChromiumCookie& cookie)
{
    auto database = openDatabase(databasePath);
    execute(database.get(), "DELETE FROM cookies");
    insertChromiumCookie(database.get(), cookie);
}

QString makeFirefoxProfile(const QString& root, const QByteArray& token, const QByteArray& host = "soundcloud.com",
                           qint64 expiry = 0)
{
    const QString databasePath = root + QStringLiteral("/default-profile/cookies.sqlite");
    auto database = openDatabase(databasePath);
    // moz_cookies as Firefox creates it (schema version 14+).
    execute(database.get(),
            "CREATE TABLE moz_cookies(id INTEGER PRIMARY KEY, originAttributes TEXT NOT NULL DEFAULT '',"
            " name TEXT, value TEXT, host TEXT, path TEXT DEFAULT '/', expiry INTEGER, lastAccessed INTEGER,"
            " creationTime INTEGER DEFAULT 0, isSecure INTEGER DEFAULT 1, isHttpOnly INTEGER DEFAULT 0,"
            " inBrowserElement INTEGER DEFAULT 0, sameSite INTEGER DEFAULT 0, schemeMap INTEGER DEFAULT 0,"
            " isPartitionedAttributeSet INTEGER DEFAULT 0,"
            " CONSTRAINT moz_uniqueid UNIQUE (name, host, path, originAttributes))");
    sqlite3_stmt* statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(database.get(),
                               "INSERT INTO moz_cookies(host,name,value,expiry,lastAccessed) VALUES(?,?,?,?,100)", -1,
                               &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_bind_text(statement, 1, host.constData(), static_cast<int>(host.size()), SQLITE_STATIC) ==
            SQLITE_OK);
    REQUIRE(sqlite3_bind_text(statement, 2, "oauth_token", -1, SQLITE_STATIC) == SQLITE_OK);
    REQUIRE(sqlite3_bind_text(statement, 3, token.constData(), static_cast<int>(token.size()), SQLITE_STATIC) ==
            SQLITE_OK);
    REQUIRE(sqlite3_bind_int64(statement, 4, expiry) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_DONE);
    REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
    database.reset();

    QFile settings(root + QStringLiteral("/profiles.ini"));
    REQUIRE(settings.open(QIODevice::WriteOnly));
    REQUIRE(settings.write("[Profile0]\nName=Default\nIsRelative=1\nPath=default-profile\n") > 0);
    return databasePath;
}

// Runs `fill` to add enough unrelated cookies to span many pages, then overwrites one table page with garbage so the
// database still opens and prepares but stepping through the query hits SQLITE_CORRUPT: the page holding `marker`
// (a value stored only in the table row), or the last page of the file when no marker is given.
void corruptCookieTable(const QString& databasePath, const char* fill, QByteArrayView marker = {})
{
    {
        auto database = openDatabase(databasePath);
        execute(database.get(), fill);
    }
    QFile file(databasePath);
    REQUIRE(file.open(QIODevice::ReadWrite));
    constexpr qint64 pageSize = 4096;
    const QByteArray contents = file.readAll();
    REQUIRE(contents.size() >= 16 * pageSize);
    qint64 page = (contents.size() / pageSize) - 1;
    if (!marker.isEmpty()) {
        const qint64 offset = contents.indexOf(marker);
        REQUIRE(offset >= pageSize);
        page = offset / pageSize;
    }
    REQUIRE(file.seek(page * pageSize));
    REQUIRE(file.write(QByteArray(pageSize, '\xff')) == pageSize);
}

// A password provider that counts how often it is asked and returns a fixed answer.
struct FakeKeyring {
    Result<std::optional<QByteArray>> answer = std::nullopt;
    int calls = 0;

    auth::ChromiumPasswordProvider provider()
    {
        return [this](const auth::BrowserProfile&) {
            ++calls;
            return answer;
        };
    }
};

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

auth::BrowserProfile chromiumProfile(const QString& databasePath)
{
    return {QStringLiteral("test"),        QStringLiteral("Chromium"),   QStringLiteral("Default"),
            auth::BrowserFamily::Chromium, auth::BrowserInstall::Native, databasePath,
            QStringLiteral("Chromium")};
}

QByteArray fileContents(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

qint64 chromeTime(qint64 secondsSinceEpoch)
{
    constexpr qint64 chromeEpochOffset = 11644473600LL;
    return (secondsSinceEpoch + chromeEpochOffset) * 1000000LL;
}

}  // namespace

TEST_SUITE("auth.browser-cookies")
{
    TEST_CASE("discovers Chromium metadata without decrypting and imports only after selection")
    {
        QTemporaryDir home;
        const QString root = home.path() + QStringLiteral("/.config/chromium");
        const QString database = makeChromiumProfile(
            root, {.encrypted = encryptChromium("browser-token", "keyring-password", "v11", "soundcloud.com")});
        const QByteArray before = fileContents(database);
        FakeKeyring keyring{.answer = std::optional<QByteArray>("keyring-password")};
        auth::BrowserCookieSource source(home.path(), keyring.provider());

        auto profiles = discover(source);
        REQUIRE(profiles);
        REQUIRE(profiles->size() == 1);
        CHECK(profiles->constFirst().browserName == u"Chromium");
        CHECK(profiles->constFirst().profileName == u"Personal");
        CHECK(profiles->constFirst().keyringName == u"Chromium");
        CHECK(keyring.calls == 0);

        auto token = importToken(source, profiles->constFirst());
        REQUIRE(token);
        CHECK(*token == QByteArrayLiteral("browser-token"));
        CHECK(keyring.calls == 1);
        CHECK(fileContents(database) == before);
    }

    TEST_CASE("v10 cookies use the built-in password even when the keyring holds one")
    {
        QTemporaryDir home;
        const QString database =
            makeChromiumProfile(home.path() + QStringLiteral("/.config/chromium"),
                                {.encrypted = encryptChromium("v10-token", "peanuts", "v10", "soundcloud.com")});
        FakeKeyring keyring{.answer = std::optional<QByteArray>("keyring-password")};
        auth::BrowserCookieSource source(home.path(), keyring.provider());
        auto token = importToken(source, chromiumProfile(database));
        REQUIRE(token);
        CHECK(*token == QByteArrayLiteral("v10-token"));
        CHECK(keyring.calls == 0);  // no keyring prompt for a value that does not need it
    }

    TEST_CASE("strips the domain hash only from version 24 databases")
    {
        QTemporaryDir home;
        const QString root = home.path() + QStringLiteral("/.config/chromium");
        const QString database =
            makeChromiumProfile(root, {.encrypted = encryptChromium("old-token", "peanuts")}, /*version=*/23);
        auth::BrowserCookieSource source(home.path(), {});
        auto token = importToken(source, chromiumProfile(database));
        REQUIRE(token);
        CHECK(*token == QByteArrayLiteral("old-token"));

        // A version 24 value whose hash belongs to another domain was not written for this cookie.
        auto handle = openDatabase(database);
        setChromiumVersion(handle.get(), 24);
        handle.reset();
        replaceChromiumCookies(database, {.encrypted = encryptChromium("moved", "peanuts", "v10", "example.com")});
        CHECK(test::errorOf(importToken(source, chromiumProfile(database))) == ErrorCode::PermissionDenied);

        replaceChromiumCookies(
            database,
            {.host = ".soundcloud.com", .encrypted = encryptChromium("dot-host", "peanuts", "v10", ".soundcloud.com")});
        token = importToken(source, chromiumProfile(database));
        REQUIRE(token);
        CHECK(*token == QByteArrayLiteral("dot-host"));
    }

    TEST_CASE("v11 without a keyring password falls back to the empty password")
    {
        QTemporaryDir home;
        const QString database =
            makeChromiumProfile(home.path() + QStringLiteral("/.config/chromium"),
                                {.encrypted = encryptChromium("empty-key", "", "v11", "soundcloud.com")});
        FakeKeyring keyring;  // the secret store holds nothing
        auth::BrowserCookieSource source(home.path(), keyring.provider());
        auto token = importToken(source, chromiumProfile(database));
        REQUIRE(token);
        CHECK(*token == QByteArrayLiteral("empty-key"));
    }

    TEST_CASE("v11 failures report the key as unavailable, passing on keyring errors")
    {
        QTemporaryDir home;
        const QString database =
            makeChromiumProfile(home.path() + QStringLiteral("/.config/chromium"),
                                {.encrypted = encryptChromium("secret", "right", "v11", "soundcloud.com")});
        SUBCASE("wrong password")
        {
            FakeKeyring keyring{.answer = std::optional<QByteArray>("wrong")};
            auth::BrowserCookieSource source(home.path(), keyring.provider());
            CHECK(test::errorOf(importToken(source, chromiumProfile(database))) == ErrorCode::PermissionDenied);
        }
        SUBCASE("no password")
        {
            FakeKeyring keyring;
            auth::BrowserCookieSource source(home.path(), keyring.provider());
            CHECK(test::errorOf(importToken(source, chromiumProfile(database))) == ErrorCode::PermissionDenied);
        }
        SUBCASE("locked keyring")
        {
            // A code the generic "cannot decrypt" fallback never uses, so only a passed-on error matches.
            FakeKeyring keyring{.answer = fail(ErrorCode::Unsupported, "locked")};
            auth::BrowserCookieSource source(home.path(), keyring.provider());
            CHECK(test::errorOf(importToken(source, chromiumProfile(database))) == ErrorCode::Unsupported);
            CHECK(keyring.calls == 1);
        }
    }

    TEST_CASE("asks the keyring once per import and prefers the most recently used cookie")
    {
        QTemporaryDir home;
        const QString database = makeChromiumProfile(home.path() + QStringLiteral("/.config/chromium"));
        auto handle = openDatabase(database);
        insertChromiumCookie(handle.get(),
                             {.encrypted = encryptChromium("older", "pw", "v11", "soundcloud.com"), .lastAccess = 100});
        insertChromiumCookie(handle.get(),
                             {.host = "api-auth.soundcloud.com",
                              .encrypted = encryptChromium("newer", "pw", "v11", "api-auth.soundcloud.com"),
                              .lastAccess = 200});
        handle.reset();
        FakeKeyring keyring{.answer = std::optional<QByteArray>("pw")};
        auth::BrowserCookieSource source(home.path(), keyring.provider());
        auto token = importToken(source, chromiumProfile(database));
        REQUIRE(token);
        CHECK(*token == QByteArrayLiteral("newer"));
        CHECK(keyring.calls == 1);
    }

    TEST_CASE("skips expired cookies and reads unencrypted values")
    {
        QTemporaryDir home;
        const QString database = makeChromiumProfile(home.path() + QStringLiteral("/.config/chromium"));
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        auto handle = openDatabase(database);
        insertChromiumCookie(handle.get(), {.encrypted = encryptChromium("expired", "peanuts", "v10", "soundcloud.com"),
                                            .expires = chromeTime(now - 60),
                                            .lastAccess = 300});
        insertChromiumCookie(handle.get(),
                             {.plain = "plain-token", .expires = chromeTime(now + 3600), .lastAccess = 200});
        handle.reset();
        auth::BrowserCookieSource source(home.path(), {});
        auto token = importToken(source, chromiumProfile(database));
        REQUIRE(token);
        CHECK(*token == QByteArrayLiteral("plain-token"));
    }

    TEST_CASE("rejects unrelated hosts and unknown encryption versions")
    {
        QTemporaryDir home;
        const QString database = makeChromiumProfile(
            home.path() + QStringLiteral("/.config/chromium"),
            {.host = "example.com", .encrypted = encryptChromium("not-soundcloud", "peanuts", "v10", "example.com")});
        auth::BrowserCookieSource source(home.path(), {});
        CHECK(test::errorOf(importToken(source, chromiumProfile(database))) == ErrorCode::NotFound);

        // Look-alike domains must not match "%.soundcloud.com".
        replaceChromiumCookies(database, {.host = "evilsoundcloud.com",
                                          .encrypted = encryptChromium("x", "peanuts", "v10", "evilsoundcloud.com")});
        CHECK(test::errorOf(importToken(source, chromiumProfile(database))) == ErrorCode::NotFound);

        replaceChromiumCookies(database, {.encrypted = QByteArrayLiteral("v20-app-bound-encryption-payload")});
        CHECK(test::errorOf(importToken(source, chromiumProfile(database))) == ErrorCode::Unsupported);
    }

    TEST_CASE("discovers Flatpak, Snap and profile-less Opera layouts")
    {
        QTemporaryDir home;
        makeChromiumProfile(home.path() + QStringLiteral("/.var/app/org.chromium.Chromium/config/chromium"));
        makeChromiumProfile(home.path() + QStringLiteral("/snap/chromium/common/chromium"));
        makeChromiumProfile(home.path() + QStringLiteral("/snap/brave/current/.config/BraveSoftware/Brave-Browser"));
        // Opera keeps Cookies directly in its user-data directory.
        const QString opera = home.path() + QStringLiteral("/.config/opera/Cookies");
        auto handle = openDatabase(opera);
        execute(handle.get(), kChromiumSchema);
        handle.reset();

        auth::BrowserCookieSource source(home.path(), {});
        auto profiles = discover(source);
        REQUIRE(profiles);
        REQUIRE(profiles->size() == 4);
        CHECK(profiles->at(0).install == auth::BrowserInstall::Flatpak);
        CHECK(profiles->at(1).install == auth::BrowserInstall::Snap);
        CHECK(profiles->at(2).browserName == u"Brave");
        CHECK(profiles->at(2).install == auth::BrowserInstall::Snap);
        CHECK(profiles->at(2).keyringName == u"Brave");
        CHECK(profiles->at(3).browserName == u"Opera");
        CHECK(profiles->at(3).profileName.isEmpty());
        CHECK(profiles->at(3).databasePath == QFileInfo(opera).absoluteFilePath());
        CHECK(profiles->at(3).keyringName == u"Chromium");  // Opera and Edge reuse Chromium's entry on Linux
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

    TEST_CASE("discovers Firefox profiles whose name contains a comma")
    {
        QTemporaryDir home;
        const QString root = home.path() + QStringLiteral("/.mozilla/firefox");
        makeFirefoxProfile(root, QByteArrayLiteral("comma-token"));
        // Firefox names the directory "<salt>.<profile name>" and writes Path= and Name= unquoted.
        REQUIRE(QDir(root).rename(QStringLiteral("default-profile"), QStringLiteral("abcd.Work, Home")));
        QFile settings(root + QStringLiteral("/profiles.ini"));
        REQUIRE(settings.open(QIODevice::WriteOnly | QIODevice::Truncate));
        REQUIRE(settings.write("[General]\nStartWithLastProfile=1\n\n"
                               "[Profile0]\nName=Work, Home\nIsRelative=1\nPath=abcd.Work, Home\n") > 0);
        settings.close();

        auth::BrowserCookieSource source(home.path(), {});
        auto profiles = discover(source);
        REQUIRE(profiles);
        REQUIRE(profiles->size() == 1);
        CHECK(profiles->constFirst().profileName == u"Work, Home");
        auto token = importToken(source, profiles->constFirst());
        REQUIRE(token);
        CHECK(*token == QByteArrayLiteral("comma-token"));
    }

    TEST_CASE("Firefox expiry is understood in seconds and in milliseconds")
    {
        const qint64 past = QDateTime::currentSecsSinceEpoch() - 60;
        for (const qint64 expiry : {past, past * 1000}) {
            QTemporaryDir home;
            makeFirefoxProfile(home.path() + QStringLiteral("/.mozilla/firefox"), QByteArrayLiteral("expired"),
                               QByteArrayLiteral(".soundcloud.com"), expiry);
            auth::BrowserCookieSource source(home.path(), {});
            auto profiles = discover(source);
            REQUIRE(profiles);
            REQUIRE(profiles->size() == 1);
            CHECK(test::errorOf(importToken(source, profiles->constFirst())) == ErrorCode::NotFound);
        }
        const qint64 future = QDateTime::currentSecsSinceEpoch() + 3600;
        for (const qint64 expiry : {future, future * 1000}) {
            CAPTURE(expiry);
            QTemporaryDir home;
            makeFirefoxProfile(home.path() + QStringLiteral("/.mozilla/firefox"), QByteArrayLiteral("live"),
                               QByteArrayLiteral(".soundcloud.com"), expiry);
            auth::BrowserCookieSource source(home.path(), {});
            auto profiles = discover(source);
            REQUIRE(profiles);
            REQUIRE(profiles->size() == 1);
            auto token = importToken(source, profiles->constFirst());
            REQUIRE_MESSAGE(token, test::describe(token));
            CHECK(*token == QByteArrayLiteral("live"));
        }
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

    TEST_CASE("an unreadable cookie table is reported as corrupt, not as a missing cookie")
    {
        QTemporaryDir home;
        const QString chromium =
            makeChromiumProfile(home.path() + QStringLiteral("/.config/chromium"),
                                {.encrypted = encryptChromium("chromium-token", "peanuts", "v10", "soundcloud.com")});
        corruptCookieTable(chromium, "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 2000) "
                                     "INSERT INTO cookies(host_key, name, value, expires_utc, last_access_utc) "
                                     "SELECT 'example.com', 'pad' || i, hex(randomblob(100)), 0, i FROM n");
        auth::BrowserCookieSource source(home.path(), {});
        CHECK(test::errorOf(importToken(source, chromiumProfile(chromium))) == ErrorCode::Corrupt);

        const QString firefox =
            makeFirefoxProfile(home.path() + QStringLiteral("/.mozilla/firefox"), QByteArrayLiteral("firefox-token"));
        corruptCookieTable(firefox,
                           "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 2000) "
                           "INSERT INTO moz_cookies(host, name, value, expiry, lastAccessed) "
                           "SELECT 'example.com', 'pad' || i, hex(randomblob(100)), 0, i FROM n",
                           "firefox-token");
        auto profiles = discover(source);
        REQUIRE(profiles);
        const auto profile = std::ranges::find(*profiles, auth::BrowserFamily::Firefox, &auth::BrowserProfile::family);
        REQUIRE(profile != profiles->cend());
        CHECK(test::errorOf(importToken(source, *profile)) == ErrorCode::Corrupt);
    }

    // Opt-in (--no-skip) because it reads the current user's real browser profiles and, for Chromium-family
    // profiles, their keyring; never prints cookie values.
    TEST_CASE("real browser profile import" * doctest::skip())
    {
        auth::BrowserCookieSource source;
        auto profiles = discover(source);
        REQUIRE(profiles);
        bool imported = false;
        for (const auto& profile : *profiles) {
            auto token = importToken(source, profile);
            MESSAGE(profile.browserName.toStdString() << " / " << profile.profileName.toStdString() << ": "
                                                      << (token ? "token found" : test::describe(token)));
            imported = imported || (token && !token->isEmpty());
        }
        CHECK(imported);
    }

    TEST_CASE("reads a committed WAL without modifying the live database")
    {
        QTemporaryDir home;
        const QString root = home.path() + QStringLiteral("/.config/chromium");
        const QString path = root + QStringLiteral("/Default/Network/Cookies");
        auto database = openDatabase(path);
        execute(database.get(), "PRAGMA journal_mode=WAL");
        execute(database.get(), kChromiumSchema);
        setChromiumVersion(database.get(), 24);
        insertChromiumCookie(database.get(),
                             {.encrypted = encryptChromium("wal-token", "peanuts", "v10", "soundcloud.com")});
        QFileInfo wal(path + QStringLiteral("-wal"));
        REQUIRE(wal.exists());
        const QByteArray walBefore = fileContents(wal.absoluteFilePath());

        auth::BrowserCookieSource source(home.path(), {});
        auto token = importToken(source, chromiumProfile(path));
        REQUIRE(token);
        CHECK(*token == QByteArrayLiteral("wal-token"));
        CHECK(fileContents(wal.absoluteFilePath()) == walBefore);
    }

    TEST_CASE("reads a committed Firefox WAL")
    {
        QTemporaryDir home;
        const QString root = home.path() + QStringLiteral("/.mozilla/firefox");
        const QString path = makeFirefoxProfile(root, QByteArrayLiteral("before-wal"));
        auto database = openDatabase(path);
        execute(database.get(), "PRAGMA journal_mode=WAL");
        execute(database.get(), "UPDATE moz_cookies SET value='in-wal'");
        REQUIRE(QFileInfo::exists(path + QStringLiteral("-wal")));
        auth::BrowserCookieSource source(home.path(), {});
        auto profiles = discover(source);
        REQUIRE(profiles);
        REQUIRE(profiles->size() == 1);
        auto token = importToken(source, profiles->constFirst());
        REQUIRE(token);
        CHECK(*token == QByteArrayLiteral("in-wal"));
    }
}
