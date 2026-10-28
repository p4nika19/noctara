#include "auth/BrowserCookieSource.hpp"

#include "auth/SecureBytes.hpp"

#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
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

// Chromium's Linux os_crypt (components/os_crypt/sync/os_crypt_linux.cc): "v10" values are encrypted with a key
// derived from the fixed password "peanuts", "v11" values with a key derived from the password the browser keeps
// in the desktop secret store. Both use PBKDF2-HMAC-SHA1 with the salt "saltysalt" and a SINGLE iteration (1003 is
// the macOS count), then AES-128-CBC with an IV of 16 spaces. A wrong count silently fails every decryption.
constexpr int kChromiumLinuxPbkdf2Iterations = 1;
constexpr QByteArrayView kChromiumV10Password("peanuts");
// Since cookie database version 24 (Chromium 130) the plaintext starts with SHA-256(host_key), binding each value
// to its domain.
constexpr int kChromiumDomainHashVersion = 24;
constexpr qsizetype kDomainHashSize = 32;
constexpr qsizetype kAesBlockSize = 16;
constexpr qsizetype kVersionPrefixSize = 3;

// KWallet may ask for the wallet password when it is opened; give the user time to type it.
constexpr int kDBusTimeoutMs = 5000;
constexpr int kWalletOpenTimeoutMs = 120000;

struct ChromiumBrowser {
    const char* key;
    const char* name;
    const char* nativePath;
    const char* flatpakId;
    const char* flatpakPath;
    const char* snapPath;
    // On Linux only Chrome, Chromium and Brave use their own name; Edge and Opera reuse Chromium's, Vivaldi
    // Chrome's (the browser-specific names are the macOS Keychain entries).
    const char* keyringName;
};

// Snap confines an application's $HOME to ~/snap/<name>/current, except Chromium, which keeps its profile in
// the revision-independent common/ directory.
constexpr std::array kChromiumBrowsers{
    ChromiumBrowser{"chrome", "Google Chrome", ".config/google-chrome", "com.google.Chrome", "config/google-chrome", "",
                    "Chrome"},
    ChromiumBrowser{"chromium", "Chromium", ".config/chromium", "org.chromium.Chromium", "config/chromium",
                    "chromium/common/chromium", "Chromium"},
    ChromiumBrowser{"brave", "Brave", ".config/BraveSoftware/Brave-Browser", "com.brave.Browser",
                    "config/BraveSoftware/Brave-Browser", "brave/current/.config/BraveSoftware/Brave-Browser", "Brave"},
    ChromiumBrowser{"edge", "Microsoft Edge", ".config/microsoft-edge", "com.microsoft.Edge", "config/microsoft-edge",
                    "", "Chromium"},
    ChromiumBrowser{"vivaldi", "Vivaldi", ".config/vivaldi", "com.vivaldi.Vivaldi", "config/vivaldi",
                    "vivaldi/current/.config/vivaldi", "Chrome"},
    ChromiumBrowser{"opera", "Opera", ".config/opera", "com.opera.Opera", "config/opera", "opera/current/.config/opera",
                    "Chromium"},
};

struct FirefoxBrowser {
    const char* key;
    const char* name;
    const char* nativePath;
    const char* xdgPath;
    const char* flatpakId;
    const char* flatpakPath;
    const char* snapPath;
};

constexpr std::array kFirefoxBrowsers{
    FirefoxBrowser{"firefox", "Firefox", ".mozilla/firefox", "mozilla/firefox", "org.mozilla.firefox",
                   ".mozilla/firefox", "firefox/common/.mozilla/firefox"},
    FirefoxBrowser{"librewolf", "LibreWolf", ".librewolf", "librewolf", "io.gitlab.librewolf-community", ".librewolf",
                   ""},
};

struct Root {
    QString path;
    BrowserInstall install;
};

template <class Browser> QVector<Root> rootsOf(const Browser& browser, const QString& home)
{
    QVector<Root> roots{{home + u'/' + QString::fromLatin1(browser.nativePath), BrowserInstall::Native}};
    if (*browser.flatpakId != '\0') {
        roots.append({home + QStringLiteral("/.var/app/") + QString::fromLatin1(browser.flatpakId) + u'/' +
                          QString::fromLatin1(browser.flatpakPath),
                      BrowserInstall::Flatpak});
    }
    if (*browser.snapPath != '\0') {
        roots.append({home + QStringLiteral("/snap/") + QString::fromLatin1(browser.snapPath), BrowserInstall::Snap});
    }
    return roots;
}

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
                     const Root& root)
{
    if (!QFileInfo::exists(root.path)) {
        return;
    }
    QJsonObject information;
    QFile localState(root.path + QStringLiteral("/Local State"));
    if (localState.open(QIODevice::ReadOnly)) {
        information = QJsonDocument::fromJson(localState.readAll())
                          .object()
                          .value(QStringLiteral("profile"))
                          .toObject()
                          .value(QStringLiteral("info_cache"))
                          .toObject();
    }

    const auto add = [&](const QString& profileDirectory, QString name) {
        const QString database = absolutePath(chromiumDatabase(profileDirectory));
        if (seen.contains(database)) {
            return;
        }
        seen.insert(database);
        profiles.append({QString::fromLatin1(browser.key) + u':' + profileDirectory, QString::fromLatin1(browser.name),
                         std::move(name), BrowserFamily::Chromium, root.install, database,
                         QString::fromLatin1(browser.keyringName)});
    };

    QStringList directories;
    if (!information.isEmpty()) {
        directories = information.keys();
    } else {
        directories.append(QStringLiteral("Default"));
        directories.append(QDir(root.path).entryList({QStringLiteral("Profile *")}, QDir::Dirs | QDir::NoDotAndDotDot));
    }
    std::ranges::sort(directories);
    for (const QString& directory : directories) {
        const QString profileDirectory = root.path + u'/' + directory;
        if (!QFileInfo::exists(profileDirectory)) {
            continue;
        }
        const QString name = information.value(directory).toObject().value(QStringLiteral("name")).toString();
        add(profileDirectory, name.isEmpty() ? directory : name);
    }
    // Opera keeps its single profile in the user-data directory itself, with no Default/ below it.
    if (QFileInfo::exists(root.path + QStringLiteral("/Cookies")) ||
        QFileInfo::exists(root.path + QStringLiteral("/Network/Cookies"))) {
        add(root.path, {});
    }
}

void addFirefoxRoot(QVector<BrowserProfile>& profiles, QSet<QString>& seen, const FirefoxBrowser& browser,
                    const Root& root)
{
    QSettings settings(root.path + QStringLiteral("/profiles.ini"), QSettings::IniFormat);
    const QStringList groups = settings.childGroups();
    QStringList profileGroups;
    for (const QString& group : groups) {
        if (group.startsWith(QStringLiteral("Profile"))) {
            profileGroups.append(group);
        }
    }
    // [Profile2] before [Profile10].
    std::ranges::sort(profileGroups, {}, [](const QString& group) { return group.mid(7).toInt(); });
    const auto add = [&](const QString& directory, const QString& database, QString name) {
        seen.insert(database);
        profiles.append({QString::fromLatin1(browser.key) + u':' + directory,
                         QString::fromLatin1(browser.name),
                         std::move(name),
                         BrowserFamily::Firefox,
                         root.install,
                         database,
                         {}});
    };
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
            directory = root.path + u'/' + directory;
        }
        const QString database = absolutePath(directory + QStringLiteral("/cookies.sqlite"));
        if (seen.contains(database) || !QFileInfo::exists(QFileInfo(database).absolutePath())) {
            continue;
        }
        add(directory, database, name.isEmpty() ? QFileInfo(directory).fileName() : std::move(name));
        found = true;
    }
    if (found || !QFileInfo::exists(root.path)) {
        return;
    }

    // A missing profiles.ini should not hide a standalone portable profile.
    const QString database = absolutePath(root.path + QStringLiteral("/cookies.sqlite"));
    if (QFileInfo::exists(database) && !seen.contains(database)) {
        add(root.path, database, {});
    }
}

QVector<BrowserProfile> discoverProfiles(const QString& home, const QString& configDirectory)
{
    QVector<BrowserProfile> profiles;
    QSet<QString> seen;
    for (const auto& browser : kChromiumBrowsers) {
        for (const Root& root : rootsOf(browser, home)) {
            addChromiumRoot(profiles, seen, browser, root);
        }
    }
    for (const auto& browser : kFirefoxBrowsers) {
        QVector<Root> roots = rootsOf(browser, home);
        roots.insert(1, {configDirectory + u'/' + QString::fromLatin1(browser.xdgPath), BrowserInstall::Native});
        for (const Root& root : roots) {
            addFirefoxRoot(profiles, seen, browser, root);
        }
    }
    return profiles;
}

void clearError(GError*& error) noexcept
{
    if (error != nullptr) {
        g_error_free(error);
        error = nullptr;
    }
}

Result<std::optional<QByteArray>> loadItemSecret(SecretService* service, SecretItem* item)
{
    GError* error = nullptr;
    if (secret_item_get_locked(item) != FALSE) {
        // Shows the desktop's own unlock prompt; the user has just asked for this import.
        GList* objects = g_list_append(nullptr, item);
        const gint unlocked = secret_service_unlock_sync(service, objects, nullptr, nullptr, &error);
        g_list_free(objects);
        clearError(error);
        if (unlocked < 1) {
            return fail(ErrorCode::PermissionDenied,
                        "The keyring holding the browser's Safe Storage password is locked");
        }
    }
    if (secret_item_load_secret_sync(item, nullptr, &error) == FALSE) {
        clearError(error);
        return fail(ErrorCode::PermissionDenied, "Could not read the browser's Safe Storage password from the keyring");
    }
    SecretValue* value = secret_item_get_secret(item);
    if (value == nullptr) {
        return fail(ErrorCode::PermissionDenied, "The keyring returned no Safe Storage password");
    }
    std::optional<QByteArray> password;
    gsize length = 0;
    const gchar* data = secret_value_get(value, &length);
    if (data != nullptr && length > 0) {
        password = QByteArray(data, static_cast<qsizetype>(length));
    }
    secret_value_unref(value);  // libsecret keeps the value in non-pageable memory and wipes it
    return password;
}

// Chromium stores the password with a schema whose name and attributes differ between browsers and versions, but
// always labels it "<name> Safe Storage" (the label yt-dlp and browser_cookie3 match too). libsecret cannot look
// an item up by label, so the default collection is scanned.
Result<std::optional<QByteArray>> secretServicePassword(const QString& keyringName)
{
    const QByteArray label = (keyringName + QStringLiteral(" Safe Storage")).toUtf8();
    GError* error = nullptr;
    const GObjectPtr<SecretService> service(secret_service_get_sync(SECRET_SERVICE_OPEN_SESSION, nullptr, &error));
    clearError(error);
    if (!service) {
        return std::nullopt;  // no Secret Service on this desktop
    }
    const GObjectPtr<SecretCollection> collection(secret_collection_for_alias_sync(
        service.get(), SECRET_COLLECTION_DEFAULT, SECRET_COLLECTION_LOAD_ITEMS, nullptr, &error));
    clearError(error);
    if (!collection) {
        return std::nullopt;
    }
    Result<std::optional<QByteArray>> password = std::nullopt;
    GList* items = secret_collection_get_items(collection.get());
    for (const GList* node = items; node != nullptr; node = node->next) {
        auto* item = static_cast<SecretItem*>(node->data);
        gchar* itemLabel = secret_item_get_label(item);
        const bool matches = itemLabel != nullptr && label == itemLabel;
        g_free(itemLabel);
        if (matches) {
            password = loadItemSecret(service.get(), item);
            break;
        }
    }
    g_list_free_full(items, g_object_unref);
    return password;
}

// KWallet's own D-Bus API (org.kde.KWallet), which Chromium uses on KDE Plasma (--password-store=kwallet5/6):
// the entry "<name> Safe Storage" in the folder "<name> Keys" of the network wallet. Read-only: unlike Chromium,
// a missing entry is never created.
Result<std::optional<QByteArray>> kwalletPassword(const QString& keyringName)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || bus.interface() == nullptr) {
        return std::nullopt;
    }
    const QDBusConnectionInterface* daemon = bus.interface();
    const QStringList activatable = daemon->activatableServiceNames().value();
    for (const auto& [service, path] :
         {std::pair{"org.kde.kwalletd6", "/modules/kwalletd6"}, std::pair{"org.kde.kwalletd5", "/modules/kwalletd5"}}) {
        const QString serviceName = QString::fromLatin1(service);
        if (!daemon->isServiceRegistered(serviceName).value() && !activatable.contains(serviceName)) {
            continue;
        }
        const QString application = QStringLiteral("Noctara");
        const auto call = [&](const char* method, const QVariantList& arguments, int timeout = kDBusTimeoutMs) {
            QDBusMessage message = QDBusMessage::createMethodCall(
                serviceName, QString::fromLatin1(path), QStringLiteral("org.kde.KWallet"), QString::fromLatin1(method));
            message.setArguments(arguments);
            const QDBusMessage reply = bus.call(message, QDBus::Block, timeout);
            return reply.type() == QDBusMessage::ReplyMessage ? reply.arguments().value(0) : QVariant();
        };
        if (!call("isEnabled", {}).toBool()) {
            return std::nullopt;
        }
        const QString wallet = call("networkWallet", {}).toString();
        const QVariant opened = call("open", {wallet, qlonglong{0}, application}, kWalletOpenTimeoutMs);
        if (!opened.isValid() || opened.toInt() < 0) {
            return fail(ErrorCode::PermissionDenied,
                        "KWallet did not open the wallet holding the Safe Storage password");
        }
        const int handle = opened.toInt();
        const QString folder = keyringName + QStringLiteral(" Keys");
        const QString entry = keyringName + QStringLiteral(" Safe Storage");
        std::optional<QByteArray> password;
        if (call("hasEntry", {handle, folder, entry, application}).toBool()) {
            const QString value = call("readPassword", {handle, folder, entry, application}).toString();
            if (!value.isEmpty()) {
                password = value.toUtf8();
            }
        }
        QDBusMessage close = QDBusMessage::createMethodCall(serviceName, QString::fromLatin1(path),
                                                            QStringLiteral("org.kde.KWallet"), QStringLiteral("close"));
        close.setArguments({handle, false, application});
        bus.send(close);
        return password;
    }
    return std::nullopt;
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
        // Without its WAL or rollback journal the copy would be an older (or torn) state of the database.
        for (const char* suffix : {"-wal", "-shm", "-journal"}) {
            const QString sidecar = source + QString::fromLatin1(suffix);
            if (QFileInfo::exists(sidecar) && !QFile::copy(sidecar, m_path + QString::fromLatin1(suffix))) {
                m_result = fail(ErrorCode::IoError, "Could not copy the browser cookie database journal");
                return;
            }
        }
    }

    [[nodiscard]] const Result<void>& result() const { return m_result; }
    [[nodiscard]] const QString& path() const { return m_path; }

private:
    // The copy holds every site's cookies: keep it on the per-user tmpfs ($XDG_RUNTIME_DIR, mode 0700) when there
    // is one, so it never reaches a disk.
    static QString snapshotTemplate()
    {
        const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
        const QString base = !runtime.isEmpty() && QFileInfo(runtime).isDir() ? runtime : QDir::tempPath();
        return base + QStringLiteral("/noctara-cookies-XXXXXX");
    }

    QTemporaryDir m_directory{snapshotTemplate()};
    QString m_path;
    Result<void> m_result;
};

Result<Database> openSnapshot(const QString& path)
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
    return Database(rawDatabase);
}

Result<Statement> prepare(sqlite3* database, const char* sql)
{
    sqlite3_stmt* rawStatement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &rawStatement, nullptr) != SQLITE_OK) {
        return fail(ErrorCode::ParseError,
                    std::string("Could not query copied cookie database: ") + sqlite3_errmsg(database));
    }
    return Statement(rawStatement);
}

QByteArray columnBytes(sqlite3_stmt* statement, int column)
{
    const auto* value = static_cast<const char*>(sqlite3_column_blob(statement, column));
    return {value, sqlite3_column_bytes(statement, column)};
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
    auto database = openSnapshot(snapshot.path());
    if (!database) {
        return std::unexpected(database.error());
    }
    auto selected =
        prepare(database->get(), "SELECT value, expiry FROM moz_cookies WHERE name='oauth_token' AND "
                                 "(host='soundcloud.com' OR host='.soundcloud.com' OR host LIKE '%.soundcloud.com') "
                                 "ORDER BY lastAccessed DESC");
    if (!selected) {
        return std::unexpected(selected.error());
    }
    // moz_cookies.expiry is in seconds; recent Firefox versions store milliseconds. No date in seconds reaches
    // 1e11 (the year 5138), so larger values are milliseconds.
    constexpr qint64 millisecondsThreshold = 100'000'000'000LL;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    while (sqlite3_step(selected->get()) == SQLITE_ROW) {
        qint64 expiry = sqlite3_column_int64(selected->get(), 1);
        if (expiry > millisecondsThreshold) {
            expiry /= 1000;
        }
        if (expiry > 0 && expiry <= now) {
            continue;
        }
        QByteArray token = columnBytes(selected->get(), 0);
        if (validToken(token)) {
            return token;
        }
        wipe(token);
    }
    return fail(ErrorCode::NotFound, "No usable SoundCloud OAuth cookie was found");
}

struct ChromiumKey {
    std::array<unsigned char, 16> bytes{};

    ChromiumKey() = default;
    ChromiumKey(const ChromiumKey&) = default;
    ChromiumKey(ChromiumKey&&) noexcept = default;
    ChromiumKey& operator=(const ChromiumKey&) = default;
    ChromiumKey& operator=(ChromiumKey&&) noexcept = default;
    ~ChromiumKey() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
};

std::optional<ChromiumKey> deriveChromiumKey(QByteArrayView password)
{
    if (password.size() > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    ChromiumKey key;
    constexpr std::array<unsigned char, 9> salt{'s', 'a', 'l', 't', 'y', 's', 'a', 'l', 't'};
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt.data(),
                          static_cast<int>(salt.size()), kChromiumLinuxPbkdf2Iterations, EVP_sha1(),
                          static_cast<int>(key.bytes.size()), key.bytes.data()) != 1) {
        return std::nullopt;
    }
    return key;
}

std::optional<QByteArray> decryptAesCbc(QByteArrayView ciphertext, const ChromiumKey& key)
{
    if (ciphertext.isEmpty() || ciphertext.size() % kAesBlockSize != 0 ||
        ciphertext.size() > std::numeric_limits<int>::max() - EVP_MAX_BLOCK_LENGTH) {
        return std::nullopt;
    }
    const CipherContext context(EVP_CIPHER_CTX_new());
    if (!context) {
        return std::nullopt;
    }
    std::array<unsigned char, 16> iv{};
    iv.fill(' ');
    QByteArray output(ciphertext.size() + EVP_MAX_BLOCK_LENGTH, Qt::Uninitialized);
    int firstSize = 0;
    int finalSize = 0;
    const bool ok = EVP_DecryptInit_ex(context.get(), EVP_aes_128_cbc(), nullptr, key.bytes.data(), iv.data()) == 1 &&
                    EVP_DecryptUpdate(context.get(), reinterpret_cast<unsigned char*>(output.data()), &firstSize,
                                      reinterpret_cast<const unsigned char*>(ciphertext.data()),
                                      static_cast<int>(ciphertext.size())) == 1 &&
                    EVP_DecryptFinal_ex(context.get(), reinterpret_cast<unsigned char*>(output.data()) + firstSize,
                                        &finalSize) == 1;
    if (!ok) {
        wipe(output);
        return std::nullopt;
    }
    output.truncate(firstSize + finalSize);
    return output;
}

// The token in one encrypted value, or std::nullopt when `key` does not decrypt it. A wrong AES-CBC key still
// passes the padding check about once in 256 tries; the domain hash and the token check catch that.
std::optional<QByteArray> chromiumToken(QByteArrayView ciphertext, const ChromiumKey& key, QByteArrayView host,
                                        int databaseVersion)
{
    std::optional<QByteArray> plaintext = decryptAesCbc(ciphertext, key);
    if (!plaintext) {
        return std::nullopt;
    }
    const WipeOnExit wipePlaintext(*plaintext);
    QByteArrayView value(*plaintext);
    if (databaseVersion >= kChromiumDomainHashVersion) {
        const QByteArray expected = QCryptographicHash::hash(host, QCryptographicHash::Sha256);
        if (value.size() < kDomainHashSize || value.first(kDomainHashSize) != expected) {
            return std::nullopt;
        }
        value = value.sliced(kDomainHashSize);
    }
    QByteArray token = value.toByteArray();
    if (!validToken(token)) {
        wipe(token);
        return std::nullopt;
    }
    return token;
}

int chromiumDatabaseVersion(sqlite3* database)
{
    sqlite3_stmt* rawStatement = nullptr;
    if (sqlite3_prepare_v2(database, "SELECT value FROM meta WHERE key='version'", -1, &rawStatement, nullptr) !=
        SQLITE_OK) {
        return 0;  // very old databases have no meta table
    }
    const Statement statement(rawStatement);
    return sqlite3_step(rawStatement) == SQLITE_ROW ? sqlite3_column_int(rawStatement, 0) : 0;
}

Result<QByteArray> readChromium(const BrowserProfile& profile, const ChromiumPasswordProvider& passwords)
{
    const CookieSnapshot snapshot(profile.databasePath);
    if (!snapshot.result()) {
        return std::unexpected(snapshot.result().error());
    }
    auto database = openSnapshot(snapshot.path());
    if (!database) {
        return std::unexpected(database.error());
    }
    const int version = chromiumDatabaseVersion(database->get());
    auto selected =
        prepare(database->get(), "SELECT host_key, value, encrypted_value, expires_utc FROM cookies WHERE "
                                 "name='oauth_token' AND (host_key='soundcloud.com' OR host_key='.soundcloud.com' OR "
                                 "host_key LIKE '%.soundcloud.com') ORDER BY last_access_utc DESC");
    if (!selected) {
        return std::unexpected(selected.error());
    }

    // A v10 value may also have been written with an empty password (Chromium falls back to one when its
    // secret store misbehaves), so the empty-password key is tried after the expected one.
    const std::optional<ChromiumKey> v10Key = deriveChromiumKey(kChromiumV10Password);
    const std::optional<ChromiumKey> emptyKey = deriveChromiumKey(QByteArrayView(""));
    std::optional<ChromiumKey> v11Key;
    bool askedForV11 = false;
    std::optional<Error> keyError;
    bool undecryptable = false;
    bool unsupported = false;

    constexpr qint64 chromeEpochOffset = 11644473600LL;
    const qint64 now = (QDateTime::currentSecsSinceEpoch() + chromeEpochOffset) * 1000000LL;
    sqlite3_stmt* statement = selected->get();
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const qint64 expiry = sqlite3_column_int64(statement, 3);
        if (expiry > 0 && expiry <= now) {
            continue;
        }
        const QByteArray host = columnBytes(statement, 0);
        QByteArray encrypted = columnBytes(statement, 2);
        const WipeOnExit wipeEncrypted(encrypted);
        if (encrypted.isEmpty()) {
            // Values from before cookie encryption (or written with encryption disabled) are plain text.
            QByteArray plain = columnBytes(statement, 1);
            if (validToken(plain)) {
                return plain;
            }
            wipe(plain);
            continue;
        }
        const ChromiumKey* expected = nullptr;
        if (encrypted.startsWith("v10")) {
            expected = v10Key ? &*v10Key : nullptr;
        } else if (encrypted.startsWith("v11")) {
            if (!askedForV11) {
                askedForV11 = true;
                Result<std::optional<QByteArray>> password =
                    passwords ? passwords(profile) : Result<std::optional<QByteArray>>(std::nullopt);
                if (!password) {
                    keyError = password.error();
                } else if (*password) {
                    v11Key = deriveChromiumKey(**password);
                    wipe(**password);
                }
            }
            expected = v11Key ? &*v11Key : nullptr;
        } else {
            unsupported = true;  // keep scanning: a later row may still hold a readable value
            continue;
        }
        const QByteArrayView ciphertext = QByteArrayView(encrypted).sliced(kVersionPrefixSize);
        for (const ChromiumKey* key : {expected, emptyKey ? &*emptyKey : nullptr}) {
            if (key == nullptr) {
                continue;
            }
            if (std::optional<QByteArray> token = chromiumToken(ciphertext, *key, host, version)) {
                return std::move(*token);
            }
        }
        undecryptable = true;
    }
    if (undecryptable) {
        if (keyError) {
            return std::unexpected(*keyError);
        }
        return fail(ErrorCode::PermissionDenied, "Chromium cookie decryption failed; the browser key is unavailable");
    }
    if (unsupported) {
        return fail(ErrorCode::Unsupported, "This Chromium cookie encryption version is not supported on Linux");
    }
    return fail(ErrorCode::NotFound, "No usable SoundCloud OAuth cookie was found");
}

template <class T, class Callback> void deliver(QFuture<T> future, QObject* context, Callback callback)
{
    future.then(context, [callback = std::move(callback)](T result) mutable { callback(std::move(result)); });
}

}  // namespace

Result<std::optional<QByteArray>> chromiumSafeStoragePassword(const BrowserProfile& profile)
{
    if (profile.keyringName.isEmpty()) {
        return std::nullopt;
    }
    // Chromium picks KWallet on KDE Plasma and the Secret Service elsewhere; ask the likely store first.
    const bool kde = qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains(QStringLiteral("KDE"), Qt::CaseInsensitive);
    std::optional<Error> failure;
    for (const bool useKWallet : {kde, !kde}) {
        Result<std::optional<QByteArray>> password =
            useKWallet ? kwalletPassword(profile.keyringName) : secretServicePassword(profile.keyringName);
        if (!password) {
            failure = password.error();
            continue;
        }
        if (*password) {
            return password;
        }
    }
    if (failure) {
        return std::unexpected(*failure);
    }
    return std::nullopt;
}

BrowserCookieSource::BrowserCookieSource(QObject* parent)
    : BrowserCookieSource(QDir::homePath(), chromiumSafeStoragePassword, parent)
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
                                  return readChromium(profile, passwords);
                              }),
            context, std::move(callback));
}

}  // namespace noctara::auth
