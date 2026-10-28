#include "auth/BrowserCookieSource.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcessEnvironment>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <libsecret/secret.h>
#include <limits>
#include <memory>
#include <openssl/evp.h>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <string_view>
#include <utility>

namespace noctara::auth {

namespace {

struct SqliteCloser {
    void operator()(sqlite3* database) const { sqlite3_close_v2(database); }
};
struct StatementCloser {
    void operator()(sqlite3_stmt* statement) const { sqlite3_finalize(statement); }
};
struct CipherContextCloser {
    void operator()(EVP_CIPHER_CTX* context) const { EVP_CIPHER_CTX_free(context); }
};
struct GObjectCloser {
    void operator()(void* object) const
    {
        if (object != nullptr) {
            g_object_unref(object);
        }
    }
};
using Database = std::unique_ptr<sqlite3, SqliteCloser>;
using Statement = std::unique_ptr<sqlite3_stmt, StatementCloser>;
using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, CipherContextCloser>;
template <class T> using GObjectPtr = std::unique_ptr<T, GObjectCloser>;

// Chromium on Linux derives the cookie key with a SINGLE PBKDF2 iteration (components/os_crypt,
// kEncryptionIterations = 1 on Linux; the 1003 value is macOS only). The wrong count silently fails every
// decryption, so this is deliberately 1.
constexpr int kChromiumLinuxPbkdf2Iterations = 1;

struct ChromiumBrowser {
    const char* key;
    const char* name;
    const char* nativePath;
    const char* flatpakId;
    const char* flatpakPath;
    const char* snapPath;
    const char* safeStorageApplication;
};

constexpr std::array kChromiumBrowsers{
    ChromiumBrowser{"chrome", "Google Chrome", ".config/google-chrome", "com.google.Chrome", "config/google-chrome", "",
                    "Chrome Safe Storage"},
    ChromiumBrowser{"chromium", "Chromium", ".config/chromium", "org.chromium.Chromium", "config/chromium",
                    "chromium/common/chromium", "Chromium Safe Storage"},
    ChromiumBrowser{"brave", "Brave", ".config/BraveSoftware/Brave-Browser", "com.brave.Browser",
                    "config/BraveSoftware/Brave-Browser", "", "Brave Safe Storage"},
    ChromiumBrowser{"edge", "Microsoft Edge", ".config/microsoft-edge", "com.microsoft.Edge", "config/microsoft-edge",
                    "", "Microsoft Edge Safe Storage"},
    ChromiumBrowser{"vivaldi", "Vivaldi", ".config/vivaldi", "com.vivaldi.Vivaldi", "config/vivaldi", "",
                    "Vivaldi Safe Storage"},
    ChromiumBrowser{"opera", "Opera", ".config/opera", "com.opera.Opera", "config/opera", "", "Opera Safe Storage"},
};

struct FirefoxBrowser {
    const char* key;
    const char* name;
    const char* nativePath;
    const char* flatpakId;
    const char* flatpakPath;
    const char* snapPath;
};

constexpr std::array kFirefoxBrowsers{
    FirefoxBrowser{"firefox", "Firefox", ".mozilla/firefox", "org.mozilla.firefox", ".mozilla/firefox",
                   "firefox/common/.mozilla/firefox"},
    FirefoxBrowser{"librewolf", "LibreWolf", ".librewolf", "io.gitlab.librewolf-community", ".librewolf", ""},
};

QString absolutePath(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString chromiumDatabase(const QString& profileDirectory)
{
    const QString network = profileDirectory + QStringLiteral("/Network/Cookies");
    return QFileInfo::exists(network) ? network : profileDirectory + QStringLiteral("/Cookies");
}

void addChromiumRoot(QVector<BrowserProfile>& profiles, QSet<QString>& seen, const ChromiumBrowser& browser,
                     const QString& root)
{
    if (!QFileInfo::exists(root)) {
        return;
    }
    QJsonObject information;
    QFile localState(root + QStringLiteral("/Local State"));
    if (localState.open(QIODevice::ReadOnly)) {
        information = QJsonDocument::fromJson(localState.readAll())
                          .object()
                          .value(QStringLiteral("profile"))
                          .toObject()
                          .value(QStringLiteral("info_cache"))
                          .toObject();
    }

    QStringList directories;
    if (!information.isEmpty()) {
        directories = information.keys();
    } else {
        directories.append(QStringLiteral("Default"));
        directories.append(QDir(root).entryList({QStringLiteral("Profile *")}, QDir::Dirs | QDir::NoDotAndDotDot));
    }
    std::ranges::sort(directories);
    for (const QString& directory : directories) {
        const QString profileDirectory = root + u'/' + directory;
        if (!QFileInfo::exists(profileDirectory)) {
            continue;
        }
        const QString database = absolutePath(chromiumDatabase(profileDirectory));
        if (seen.contains(database)) {
            continue;
        }
        seen.insert(database);
        QString name = information.value(directory).toObject().value(QStringLiteral("name")).toString();
        if (name.isEmpty()) {
            name = directory;
        }
        profiles.append({QString::fromLatin1(browser.key) + u':' + profileDirectory, QString::fromLatin1(browser.name),
                         name, BrowserFamily::Chromium, database, QString::fromLatin1(browser.safeStorageApplication)});
    }
}

void addFirefoxRoot(QVector<BrowserProfile>& profiles, QSet<QString>& seen, const FirefoxBrowser& browser,
                    const QString& root)
{
    QSettings settings(root + QStringLiteral("/profiles.ini"), QSettings::IniFormat);
    const QStringList groups = settings.childGroups();
    QStringList profileGroups;
    for (const QString& group : groups) {
        if (group.startsWith(QStringLiteral("Profile"))) {
            profileGroups.append(group);
        }
    }
    std::ranges::sort(profileGroups);
    bool found = false;
    for (const QString& group : profileGroups) {
        settings.beginGroup(group);
        QString directory = settings.value(QStringLiteral("Path")).toString();
        const bool relative = settings.value(QStringLiteral("IsRelative"), 1).toInt() != 0;
        QString name = settings.value(QStringLiteral("Name")).toString();
        settings.endGroup();
        if (directory.isEmpty()) {
            continue;
        }
        if (relative) {
            directory = root + u'/' + directory;
        }
        const QString database = absolutePath(directory + QStringLiteral("/cookies.sqlite"));
        if (seen.contains(database) || !QFileInfo::exists(QFileInfo(database).absolutePath())) {
            continue;
        }
        seen.insert(database);
        if (name.isEmpty()) {
            name = QFileInfo(directory).fileName();
        }
        profiles.append({QString::fromLatin1(browser.key) + u':' + directory,
                         QString::fromLatin1(browser.name),
                         name,
                         BrowserFamily::Firefox,
                         database,
                         {}});
        found = true;
    }
    if (found || !QFileInfo::exists(root)) {
        return;
    }

    // A missing profiles.ini should not hide a standalone portable profile.
    const QString database = absolutePath(root + QStringLiteral("/cookies.sqlite"));
    if (QFileInfo::exists(database) && !seen.contains(database)) {
        seen.insert(database);
        profiles.append({QString::fromLatin1(browser.key) + u':' + root,
                         QString::fromLatin1(browser.name),
                         QStringLiteral("Default"),
                         BrowserFamily::Firefox,
                         database,
                         {}});
    }
}

QVector<BrowserProfile> discoverProfiles(const QString& home, const QString& configDirectory)
{
    QVector<BrowserProfile> profiles;
    QSet<QString> seen;
    for (const auto& browser : kChromiumBrowsers) {
        addChromiumRoot(profiles, seen, browser, home + u'/' + QString::fromLatin1(browser.nativePath));
        if (*browser.flatpakId != '\0') {
            addChromiumRoot(profiles, seen, browser,
                            home + QStringLiteral("/.var/app/") + QString::fromLatin1(browser.flatpakId) + u'/' +
                                QString::fromLatin1(browser.flatpakPath));
        }
        if (*browser.snapPath != '\0') {
            addChromiumRoot(profiles, seen, browser,
                            home + QStringLiteral("/snap/") + QString::fromLatin1(browser.snapPath));
        }
    }
    for (const auto& browser : kFirefoxBrowsers) {
        addFirefoxRoot(profiles, seen, browser, home + u'/' + QString::fromLatin1(browser.nativePath));
        const QString xdgPath = browser.key == std::string_view("firefox") ? QStringLiteral("/mozilla/firefox")
                                                                           : QStringLiteral("/librewolf");
        addFirefoxRoot(profiles, seen, browser, configDirectory + xdgPath);
        addFirefoxRoot(profiles, seen, browser,
                       home + QStringLiteral("/.var/app/") + QString::fromLatin1(browser.flatpakId) + u'/' +
                           QString::fromLatin1(browser.flatpakPath));
        if (*browser.snapPath != '\0') {
            addFirefoxRoot(profiles, seen, browser,
                           home + QStringLiteral("/snap/") + QString::fromLatin1(browser.snapPath));
        }
    }
    return profiles;
}

Result<QByteArray> chromiumPassword(const BrowserProfile& profile)
{
    // Chromium stores its OSCrypt key in the login keyring as an item labelled "<Browser> Safe Storage" (the
    // same label yt-dlp and browser_cookie3 match; the libsecret schema name and the "application" attribute
    // value are not stable across browsers, so a name/attribute lookup misses). libsecret cannot look an item
    // up by label, so scan the default collection. If nothing matches, the browser is using the built-in
    // "peanuts" password (no keyring encryption).
    const QByteArray label = profile.safeStorageApplication.toUtf8();
    if (label.isEmpty()) {
        return QByteArrayLiteral("peanuts");
    }
    GError* error = nullptr;
    const GObjectPtr<SecretService> service(secret_service_get_sync(SECRET_SERVICE_NONE, nullptr, &error));
    if (error != nullptr) {
        g_error_free(error);
    }
    if (!service) {
        return QByteArrayLiteral("peanuts");
    }
    error = nullptr;
    const GObjectPtr<SecretCollection> collection(secret_collection_for_alias_sync(
        service.get(), SECRET_COLLECTION_DEFAULT, SECRET_COLLECTION_LOAD_ITEMS, nullptr, &error));
    if (error != nullptr) {
        g_error_free(error);
    }
    if (!collection) {
        return QByteArrayLiteral("peanuts");
    }

    QByteArray password;
    bool found = false;
    GList* items = secret_collection_get_items(collection.get());
    for (const GList* node = items; node != nullptr && !found; node = node->next) {
        auto* item = static_cast<SecretItem*>(node->data);
        gchar* itemLabel = secret_item_get_label(item);
        const bool matches = itemLabel != nullptr && label == itemLabel;
        g_free(itemLabel);
        if (!matches) {
            continue;
        }
        GError* loadError = nullptr;
        if (secret_item_load_secret_sync(item, nullptr, &loadError) == FALSE) {
            if (loadError != nullptr) {
                g_error_free(loadError);  // e.g. a locked keyring: keep scanning, then fall back
            }
            continue;
        }
        if (SecretValue* value = secret_item_get_secret(item); value != nullptr) {
            gsize length = 0;
            const gchar* data = secret_value_get(value, &length);
            if (data != nullptr && length > 0) {
                password = QByteArray(data, static_cast<qsizetype>(length));
                found = true;
            }
            secret_value_unref(value);
        }
    }
    g_list_free_full(items, g_object_unref);

    if (!found) {
        return QByteArrayLiteral("peanuts");
    }
    if (password.isEmpty()) {
        return fail(ErrorCode::PermissionDenied, "Chromium Safe Storage password is empty");
    }
    return password;
}

class CookieSnapshot {
public:
    explicit CookieSnapshot(const QString& source)
    {
        if (!QFileInfo::exists(source)) {
            m_result = fail(ErrorCode::NotFound, "Browser cookie database does not exist");
            return;
        }
        if (!m_directory.isValid()) {
            m_result = fail(ErrorCode::IoError, "Could not create a private cookie snapshot directory");
            return;
        }
        m_path = m_directory.filePath(QStringLiteral("cookies.sqlite"));
        if (!QFile::copy(source, m_path)) {
            m_result = fail(ErrorCode::IoError, "Could not copy the browser cookie database");
            return;
        }
        for (const char* suffix : {"-wal", "-shm"}) {
            const QString sidecar = source + QString::fromLatin1(suffix);
            if (QFileInfo::exists(sidecar)) {
                (void)QFile::copy(sidecar, m_path + QString::fromLatin1(suffix));
            }
        }
    }

    [[nodiscard]] const Result<void>& result() const { return m_result; }
    [[nodiscard]] const QString& path() const { return m_path; }

private:
    QTemporaryDir m_directory;
    QString m_path;
    Result<void> m_result;
};

Result<std::pair<Database, Statement>> query(const QString& path, const char* sql)
{
    sqlite3* rawDatabase = nullptr;
    const QByteArray native = QFile::encodeName(path);
    if (sqlite3_open_v2(native.constData(), &rawDatabase, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) !=
        SQLITE_OK) {
        const std::string detail = rawDatabase != nullptr ? sqlite3_errmsg(rawDatabase) : "sqlite open failed";
        if (rawDatabase != nullptr) {
            sqlite3_close_v2(rawDatabase);
        }
        return fail(ErrorCode::Corrupt, "Could not open copied cookie database: " + detail);
    }
    Database database(rawDatabase);
    sqlite3_stmt* rawStatement = nullptr;
    if (sqlite3_prepare_v2(database.get(), sql, -1, &rawStatement, nullptr) != SQLITE_OK) {
        return fail(ErrorCode::ParseError,
                    std::string("Could not query copied cookie database: ") + sqlite3_errmsg(database.get()));
    }
    return std::make_pair(std::move(database), Statement(rawStatement));
}

bool validToken(const QByteArray& token)
{
    return !token.isEmpty() && token.size() <= 4096 && std::ranges::all_of(token, [](const char value) {
        const auto byte = static_cast<unsigned char>(value);
        return byte >= 0x21 && byte <= 0x7e;
    });
}

Result<QByteArray> readFirefox(const QString& path)
{
    const CookieSnapshot snapshot(path);
    if (!snapshot.result()) {
        return std::unexpected(snapshot.result().error());
    }
    auto selected =
        query(snapshot.path(), "SELECT value, expiry FROM moz_cookies WHERE name='oauth_token' AND "
                               "(host='soundcloud.com' OR host='.soundcloud.com' OR host LIKE '%.soundcloud.com') "
                               "ORDER BY lastAccessed DESC");
    if (!selected) {
        return std::unexpected(selected.error());
    }
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    while (sqlite3_step(selected->second.get()) == SQLITE_ROW) {
        const qint64 expiry = sqlite3_column_int64(selected->second.get(), 1);
        if (expiry > 0 && expiry <= now) {
            continue;
        }
        const auto* value = static_cast<const char*>(sqlite3_column_blob(selected->second.get(), 0));
        const int size = sqlite3_column_bytes(selected->second.get(), 0);
        QByteArray token(value, size);
        if (validToken(token)) {
            return token;
        }
    }
    return fail(ErrorCode::NotFound, "No usable SoundCloud OAuth cookie was found");
}

Result<QByteArray> decryptChromiumV10(const QByteArray& encrypted, const QByteArray& password)
{
    constexpr qsizetype aesBlockSize = 16;
    if (!encrypted.startsWith("v10") || encrypted.size() <= 3 || (encrypted.size() - 3) % aesBlockSize != 0) {
        return fail(ErrorCode::Corrupt, "Invalid Chromium v10 cookie payload");
    }
    if (password.size() > std::numeric_limits<int>::max() || encrypted.size() - 3 > std::numeric_limits<int>::max()) {
        return fail(ErrorCode::InvalidArgument, "Chromium cookie input is too large");
    }
    std::array<unsigned char, 16> key{};
    constexpr std::array<unsigned char, 9> salt{'s', 'a', 'l', 't', 'y', 's', 'a', 'l', 't'};
    if (PKCS5_PBKDF2_HMAC(password.constData(), static_cast<int>(password.size()), salt.data(),
                          static_cast<int>(salt.size()), kChromiumLinuxPbkdf2Iterations, EVP_sha1(),
                          static_cast<int>(key.size()), key.data()) != 1) {
        return fail(ErrorCode::Unsupported, "Could not derive the Chromium cookie key");
    }

    const CipherContext context(EVP_CIPHER_CTX_new());
    if (!context) {
        OPENSSL_cleanse(key.data(), key.size());
        return fail(ErrorCode::Unknown, "Could not allocate the Chromium cipher context");
    }
    std::array<unsigned char, 16> iv{};
    iv.fill(' ');
    QByteArray output(encrypted.size() - 3 + EVP_MAX_BLOCK_LENGTH, Qt::Uninitialized);
    int firstSize = 0;
    int finalSize = 0;
    const auto* ciphertext = reinterpret_cast<const unsigned char*>(encrypted.constData() + 3);
    const int ciphertextSize = static_cast<int>(encrypted.size() - 3);
    const bool ok = EVP_DecryptInit_ex(context.get(), EVP_aes_128_cbc(), nullptr, key.data(), iv.data()) == 1 &&
                    EVP_DecryptUpdate(context.get(), reinterpret_cast<unsigned char*>(output.data()), &firstSize,
                                      ciphertext, ciphertextSize) == 1 &&
                    EVP_DecryptFinal_ex(context.get(), reinterpret_cast<unsigned char*>(output.data()) + firstSize,
                                        &finalSize) == 1;
    OPENSSL_cleanse(key.data(), key.size());
    if (!ok) {
        OPENSSL_cleanse(output.data(), static_cast<std::size_t>(output.size()));
        return fail(ErrorCode::PermissionDenied, "Chromium cookie decryption failed; the browser key is unavailable");
    }
    output.truncate(firstSize + finalSize);
    if (!validToken(output)) {
        OPENSSL_cleanse(output.data(), static_cast<std::size_t>(output.size()));
        return fail(ErrorCode::Corrupt, "Decrypted SoundCloud cookie is not a valid token");
    }
    return output;
}

Result<QByteArray> readChromium(const BrowserProfile& profile, const ChromiumPasswordProvider& passwords)
{
    const CookieSnapshot snapshot(profile.databasePath);
    if (!snapshot.result()) {
        return std::unexpected(snapshot.result().error());
    }
    auto selected =
        query(snapshot.path(), "SELECT encrypted_value, expires_utc FROM cookies WHERE name='oauth_token' AND "
                               "(host_key='soundcloud.com' OR host_key='.soundcloud.com' OR "
                               "host_key LIKE '%.soundcloud.com') ORDER BY last_access_utc DESC");
    if (!selected) {
        return std::unexpected(selected.error());
    }
    std::optional<QByteArray> encrypted;
    bool sawUnsupported = false;
    constexpr qint64 chromeEpochOffset = 11644473600LL;
    const qint64 now = (QDateTime::currentSecsSinceEpoch() + chromeEpochOffset) * 1000000LL;
    while (sqlite3_step(selected->second.get()) == SQLITE_ROW) {
        const qint64 expiry = sqlite3_column_int64(selected->second.get(), 1);
        if (expiry > 0 && expiry <= now) {
            continue;
        }
        const auto* value = static_cast<const char*>(sqlite3_column_blob(selected->second.get(), 0));
        const int size = sqlite3_column_bytes(selected->second.get(), 0);
        QByteArray candidate(value, size);
        if (candidate.startsWith("v10")) {
            encrypted = std::move(candidate);
            break;
        }
        if (candidate.size() >= 3) {
            sawUnsupported = true;  // keep scanning: a later row may still hold a usable v10 cookie
        }
    }
    if (!encrypted) {
        return sawUnsupported
                   ? fail(ErrorCode::Unsupported, "This Chromium cookie encryption version is not supported on Linux")
                   : fail(ErrorCode::NotFound, "No usable SoundCloud OAuth cookie was found");
    }
    Result<QByteArray> password = passwords(profile);
    if (!password) {
        return std::unexpected(password.error());
    }
    return decryptChromiumV10(*encrypted, *password);
}

template <class T, class Callback> void deliver(QFuture<T> future, QObject* context, Callback callback)
{
    future.then(context, [callback = std::move(callback)](const T& result) { callback(result); });
}

}  // namespace

BrowserCookieSource::BrowserCookieSource(QObject* parent)
    : BrowserCookieSource(QDir::homePath(), chromiumPassword, parent)
{
    const QString xdgConfig = QProcessEnvironment::systemEnvironment().value(QStringLiteral("XDG_CONFIG_HOME"));
    if (!xdgConfig.isEmpty()) {
        m_configDirectory = xdgConfig;
    }
}

BrowserCookieSource::BrowserCookieSource(QString homeDirectory, ChromiumPasswordProvider passwords, QObject* parent)
    : QObject(parent)
    , m_homeDirectory(std::move(homeDirectory))
    , m_configDirectory(m_homeDirectory + QStringLiteral("/.config"))
    , m_passwords(std::move(passwords))
{
    m_pool.setMaxThreadCount(1);
}

BrowserCookieSource::~BrowserCookieSource()
{
    m_pool.waitForDone();
}

void BrowserCookieSource::discover(QObject* context, DiscoverCallback callback)
{
    deliver(QtConcurrent::run(&m_pool, [home = m_homeDirectory,
                                        config = m_configDirectory] { return discoverProfiles(home, config); }),
            context, std::move(callback));
}

void BrowserCookieSource::importToken(const BrowserProfile& profile, QObject* context, ImportCallback callback)
{
    deliver(QtConcurrent::run(&m_pool,
                              [profile, passwords = m_passwords] -> Result<QByteArray> {
                                  if (profile.family == BrowserFamily::Firefox) {
                                      return readFirefox(profile.databasePath);
                                  }
                                  if (!passwords) {
                                      return fail(ErrorCode::PermissionDenied,
                                                  "No Chromium Safe Storage password provider is configured");
                                  }
                                  return readChromium(profile, passwords);
                              }),
            context, std::move(callback));
}

}  // namespace noctara::auth
