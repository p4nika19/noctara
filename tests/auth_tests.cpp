// The auth layer: the in-memory secret store's round-trip, and the Accounts view-model's sign-in by token
// (verified against /me before it is kept), expiry handling and sign-out, against tests/mock/FakeApi.hpp.
#include "TestUtil.hpp"
#include "api/ApiClient.hpp"
#include "api/ApiService.hpp"
#include "api/ClientId.hpp"
#include "api/Endpoints.hpp"
#include "api/Session.hpp"
#include "api/Snapshot.hpp"
#include "mock/FakeApi.hpp"
#include "mock/MockServers.hpp"

#include "auth/SecretStore.hpp"
#include "i18n/LanguageManager.hpp"
#include "net/NetworkService.hpp"
#include "settings/ConfigFile.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"
#include "ui/AccountController.hpp"
#include "ui/CatalogController.hpp"
#include "ui/NetworkController.hpp"
#include "ui/SettingsController.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>

using namespace noctara;
using test::FakeApi;

namespace {

constexpr auto kClientId = "AbCdEfGhIjKlMnOpQrStUvWxYz012345";
constexpr auto kTokenKey = "soundcloud.oauth_token";

QByteArray meJson()
{
    return R"({"id":42,"username":"nightowl","full_name":"Night Owl","permalink":"nightowl",
        "permalink_url":"https://soundcloud.com/nightowl",
        "avatar_url":"https://i1.sndcdn.com/avatars-nightowl-large.jpg","verified":false})";
}

net::Headers jsonHeaders()
{
    return {{QByteArrayLiteral("content-type"), QByteArrayLiteral("application/json")}};
}

// An ApiClient on the test thread with a fixed client_id, plus a secret store, as the Accounts view-model sees
// them. `validToken` is the one /me accepts; any other token is answered with 401.
struct Harness {
    FakeApi transport;
    api::Session session;
    api::ClientIdProvider clientIds{transport, {}};
    api::ApiClient client{transport, session, clientIds};
    auth::MemorySecretStore secrets;

    explicit Harness(const QByteArray& validToken = "good-token")
    {
        clientIds.setOverride(QString::fromLatin1(kClientId));
        transport.setHandler([validToken](const net::Request& request) -> Result<net::Response> {
            const QByteArray auth = FakeApi::header(request, QByteArrayLiteral("authorization"));
            if (auth == "OAuth " + validToken) {
                return FakeApi::reply(200, meJson(), jsonHeaders());
            }
            return FakeApi::reply(401, R"({"error":"401 - Unauthorized"})", jsonHeaders());
        });
    }

    // Writes a token straight into the secret store, as a previous run would have left it.
    void preStore(const QByteArray& token)
    {
        secrets.write(QString::fromLatin1(kTokenKey), QStringLiteral("test"), token, &secrets,
                      [](const Result<void>&) {});
    }

    [[nodiscard]] std::optional<QByteArray> stored()
    {
        QObject ctx;
        auto out = std::make_shared<std::optional<std::optional<QByteArray>>>();
        secrets.read(QString::fromLatin1(kTokenKey), &ctx,
                     [out](Result<std::optional<QByteArray>> value) { *out = value ? *value : std::nullopt; });
        if (!test::waitUntil([&] { return out->has_value(); }, 2000)) {
            return std::nullopt;
        }
        return **out;
    }
};

// A memory store whose reads are held until release(), like a keyring that is slow to answer at startup.
class SlowSecretStore final : public QObject, public auth::ISecretStore {
public:
    auth::MemorySecretStore inner;

    void read(const QString& key, QObject* context, ReadCallback callback) override
    {
        m_held.push_back([this, key, context = QPointer(context), callback = std::move(callback)] {
            if (context != nullptr) {
                inner.read(key, context, callback);
            }
        });
    }
    void write(const QString& key, const QString& label, const QByteArray& secret, QObject* context,
               WriteCallback callback) override
    {
        inner.write(key, label, secret, context, std::move(callback));
    }
    void remove(const QString& key, QObject* context, WriteCallback callback) override
    {
        inner.remove(key, context, std::move(callback));
    }
    [[nodiscard]] bool persistent() const noexcept override { return true; }
    [[nodiscard]] bool keyringBacked() const noexcept override { return true; }

    void release()
    {
        for (auto& held : std::exchange(m_held, {})) {
            held();
        }
    }

private:
    std::vector<std::function<void()>> m_held;
};

class FakeBrowserSource final : public auth::IBrowserCookieSource {
public:
    QVector<auth::BrowserProfile> profiles{{QStringLiteral("profile-1"),
                                            QStringLiteral("Fixture Browser"),
                                            QStringLiteral("Personal"),
                                            auth::BrowserFamily::Firefox,
                                            auth::BrowserInstall::Native,
                                            QStringLiteral("/fixture/cookies.sqlite"),
                                            {}}};
    Result<QByteArray> token = QByteArrayLiteral("good-token");
    int discoveries = 0;
    int imports = 0;

    void discover(QObject* context, DiscoverCallback callback) override
    {
        ++discoveries;
        QMetaObject::invokeMethod(
            context, [callback = std::move(callback), profiles = profiles] { callback(profiles); },
            Qt::QueuedConnection);
    }

    void importToken(const auth::BrowserProfile& profile, QObject* context, ImportCallback callback) override
    {
        ++imports;
        CHECK(profile.id == u"profile-1");
        QMetaObject::invokeMethod(
            context, [callback = std::move(callback), token = token]() mutable { callback(std::move(token)); },
            Qt::QueuedConnection);
    }
};

}  // namespace

TEST_SUITE("auth.secret-store")
{
    TEST_CASE("memory store round-trips, overwrites and removes")
    {
        auth::MemorySecretStore store;
        QObject ctx;
        CHECK_FALSE(store.persistent());

        auto read = [&](const QString& key) -> std::optional<QByteArray> {
            auto out = std::make_shared<std::optional<std::optional<QByteArray>>>();
            store.read(key, &ctx, [out](Result<std::optional<QByteArray>> v) { *out = v ? *v : std::nullopt; });
            REQUIRE(test::waitUntil([&] { return out->has_value(); }, 2000));
            return **out;
        };

        CHECK_FALSE(read(QStringLiteral("k")).has_value());

        store.write(QStringLiteral("k"), QStringLiteral("label"), QByteArray("secret-1"), &ctx,
                    [](const Result<void>&) {});
        CHECK(read(QStringLiteral("k")) == QByteArray("secret-1"));

        store.write(QStringLiteral("k"), QStringLiteral("label"), QByteArray("secret-2"), &ctx,
                    [](const Result<void>&) {});
        CHECK(read(QStringLiteral("k")) == QByteArray("secret-2"));

        store.remove(QStringLiteral("k"), &ctx, [](const Result<void>&) {});
        CHECK_FALSE(read(QStringLiteral("k")).has_value());
    }
}

TEST_SUITE("ui.account")
{
    using ui::AccountController;

    TEST_CASE("signs in with a valid token, verifying against /me and saving it")
    {
        Harness h("good-token");
        AccountController account(h.client, h.session, h.secrets);
        QSignalSpy changed(&account, &AccountController::changed);

        account.signInWithToken(QStringLiteral("  good-token  "));  // surrounding whitespace is trimmed
        REQUIRE(test::waitUntil([&] { return account.signedIn(); }, 3000));

        CHECK(account.state() == AccountController::SignedIn);
        CHECK(account.username() == u"nightowl");
        CHECK(account.displayName() == u"Night Owl");
        CHECK(account.permalinkUrl() == u"https://soundcloud.com/nightowl");
        CHECK(h.session.token() == QByteArray("good-token"));
        CHECK(h.stored() == QByteArray("good-token"));  // persisted for next launch
    }

    TEST_CASE("an empty token is refused without any request")
    {
        Harness h;
        AccountController account(h.client, h.session, h.secrets);
        QSignalSpy notice(&account, &AccountController::notice);

        account.signInWithToken(QStringLiteral("   "));  // refused synchronously, before any request
        REQUIRE(notice.count() == 1);
        CHECK(notice.constFirst().constFirst().toString() == u"error");
        CHECK(account.state() == AccountController::SignedOut);
        CHECK(h.transport.count() == 0);
    }

    TEST_CASE("a rejected token leaves the session signed out and unsaved")
    {
        Harness h("good-token");
        AccountController account(h.client, h.session, h.secrets);
        QSignalSpy notice(&account, &AccountController::notice);

        account.signInWithToken(QStringLiteral("wrong-token"));
        REQUIRE(notice.wait(3000));
        CHECK(notice.constFirst().constFirst().toString() == u"error");
        CHECK(account.state() == AccountController::SignedOut);
        CHECK(h.session.token().isEmpty());
        CHECK_FALSE(h.stored().has_value());
    }

    TEST_CASE("signing out forgets the token everywhere")
    {
        Harness h("good-token");
        AccountController account(h.client, h.session, h.secrets);
        account.signInWithToken(QStringLiteral("good-token"));
        REQUIRE(test::waitUntil([&] { return account.signedIn(); }, 3000));

        account.signOut();
        CHECK(account.state() == AccountController::SignedOut);
        CHECK(h.session.token().isEmpty());
        CHECK_FALSE(h.stored().has_value());
    }

    TEST_CASE("a rejection during use expires the session and clears the saved token")
    {
        Harness h("good-token");
        AccountController account(h.client, h.session, h.secrets);
        account.signInWithToken(QStringLiteral("good-token"));
        REQUIRE(test::waitUntil([&] { return account.signedIn(); }, 3000));

        // Simulate a later request getting a 401 for the signed-in token.
        h.session.reportRejected(QByteArray("good-token"));
        REQUIRE(test::waitUntil([&] { return account.state() == AccountController::Expired; }, 2000));

        CHECK(account.sessionExpired());
        CHECK(h.session.token().isEmpty());
        CHECK_FALSE(h.stored().has_value());
    }

    TEST_CASE("a saved token is loaded and verified at construction")
    {
        Harness h("good-token");
        h.preStore(QByteArray("good-token"));

        AccountController account(h.client, h.session, h.secrets);
        REQUIRE(test::waitUntil([&] { return account.username() == u"nightowl"; }, 3000));

        CHECK(account.signedIn());
        CHECK(h.session.token() == QByteArray("good-token"));
    }

    TEST_CASE("browser discovery does not import; confirmed import verifies and saves the token")
    {
        Harness h("good-token");
        FakeBrowserSource browsers;
        AccountController account(h.client, h.session, h.secrets, &browsers);

        account.discoverBrowsers();
        REQUIRE(test::waitUntil([&] { return account.browserProfiles().size() == 1; }));
        CHECK(browsers.discoveries == 1);
        CHECK(browsers.imports == 0);  // metadata discovery alone has not touched the cookie DB
        CHECK(h.transport.count() == 0);

        account.importBrowserToken(QStringLiteral("profile-1"));
        REQUIRE(test::waitUntil([&] { return account.signedIn(); }, 3000));
        CHECK(browsers.imports == 1);
        CHECK(h.session.token() == QByteArray("good-token"));
        CHECK(h.stored() == QByteArray("good-token"));
    }

    TEST_CASE("a refused token never reaches the session, other requests or its rejection signal")
    {
        Harness h("good-token");
        AccountController account(h.client, h.session, h.secrets);
        account.signInWithToken(QStringLiteral("good-token"));
        REQUIRE(test::waitUntil([&] { return account.signedIn() && account.username() == u"nightowl"; }, 3000));

        QByteArray sessionDuringCheck;
        h.transport.setHandler([&](const net::Request& request) -> Result<net::Response> {
            sessionDuringCheck = h.session.token();
            const bool good = FakeApi::header(request, QByteArrayLiteral("authorization")) == "OAuth good-token";
            return good ? FakeApi::reply(200, meJson(), jsonHeaders())
                        : FakeApi::reply(401, R"({"error":"401 - Unauthorized"})", jsonHeaders());
        });
        QSignalSpy rejected(&h.session, &api::Session::rejected);
        QSignalSpy notice(&account, &AccountController::notice);
        account.signInWithToken(QStringLiteral("wrong-token"));
        CHECK(h.session.token() == QByteArray("good-token"));  // not installed while it is being checked
        REQUIRE(notice.wait(3000));
        CHECK(notice.constFirst().constFirst().toString() == u"error");
        CHECK(sessionDuringCheck == QByteArray("good-token"));
        CHECK(account.state() == AccountController::SignedIn);
        CHECK(h.session.token() == QByteArray("good-token"));
        CHECK(h.stored() == QByteArray("good-token"));
        CHECK(rejected.isEmpty());  // a wrong candidate says nothing about the working session
    }

    TEST_CASE("a refused candidate resumes the unconfirmed saved session's background check")
    {
        Harness h("good-token");
        h.preStore(QByteArray("good-token"));
        std::atomic<bool> online = false;
        h.transport.setHandler([&](const net::Request& request) -> Result<net::Response> {
            const QByteArray auth = FakeApi::header(request, QByteArrayLiteral("authorization"));
            if (auth != "OAuth good-token") {
                return FakeApi::reply(401, R"({"error":"401 - Unauthorized"})", jsonHeaders());
            }
            if (!online) {
                return fail(ErrorCode::ConnectionFailed, "offline");
            }
            return FakeApi::reply(200, meJson(), jsonHeaders());
        });
        const auto savedChecks = [&] {
            int count = 0;
            for (const net::Request& request : h.transport.requests()) {
                count += FakeApi::header(request, QByteArrayLiteral("authorization")) == "OAuth good-token" ? 1 : 0;
            }
            return count;
        };
        AccountController account(h.client, h.session, h.secrets);
        QSignalSpy notice(&account, &AccountController::notice);
        // The saved session is used at once; its /me fails offline and is retried much later.
        REQUIRE(test::waitUntil([&] { return account.signedIn() && savedChecks() == 1; }, 3000));
        test::waitUntil([] { return false; }, 100);
        CHECK(account.userId() == 0);

        account.signInWithToken(QStringLiteral("wrong-token"));
        REQUIRE(notice.wait(3000));
        CHECK(account.state() == AccountController::SignedIn);

        online = true;
        CHECK(test::waitUntil([&] { return savedChecks() >= 2; }, 3000));
        CHECK(test::waitUntil([&] { return account.userId() == 42; }, 3000));
        CHECK(h.session.token() == QByteArray("good-token"));
    }

    TEST_CASE("a queued rejection of the previous token leaves a newly installed token alone")
    {
        Harness h("good-token");
        AccountController account(h.client, h.session, h.secrets);
        account.signInWithToken(QStringLiteral("good-token"));
        REQUIRE(test::waitUntil([&] { return account.signedIn() && account.username() == u"nightowl"; }, 3000));

        // The API thread reports a 401 for the old token while the new token's accepted check is already queued
        // for the GUI thread: the rejection arrives after the new token is installed.
        std::thread([&] { h.session.reportRejected("good-token"); }).join();
        h.session.setToken("new-token");
        test::waitUntil([] { return false; }, 100);
        CHECK(h.session.token() == QByteArray("new-token"));
        CHECK(account.state() == AccountController::SignedIn);

        // A rejection of the current token still expires it.
        std::thread([&] { h.session.reportRejected("new-token"); }).join();
        REQUIRE(test::waitUntil([&] { return account.state() == AccountController::Expired; }, 3000));
        CHECK(h.session.token().isEmpty());
    }

    TEST_CASE("the check of a candidate token bypasses the HTTP cache")
    {
        Harness h("good-token");
        AccountController account(h.client, h.session, h.secrets);
        account.signInWithToken(QStringLiteral("good-token"));
        REQUIRE(test::waitUntil([&] { return account.signedIn(); }, 3000));
        const auto requests = h.transport.requests();
        REQUIRE(requests.size() == 1);
        CHECK(requests.front().cache == net::CacheMode::Bypass);
    }

    TEST_CASE("a token with spaces or line breaks is refused without any request")
    {
        Harness h;
        AccountController account(h.client, h.session, h.secrets);
        QSignalSpy notice(&account, &AccountController::notice);
        account.signInWithToken(QStringLiteral("good-token\r\nX-Injected: 1"));
        REQUIRE(notice.count() == 1);
        CHECK(notice.constFirst().constFirst().toString() == u"error");
        CHECK(h.transport.count() == 0);
    }

    TEST_CASE("signing out while a token is being checked is final")
    {
        Harness h("good-token");
        AccountController account(h.client, h.session, h.secrets);
        account.signInWithToken(QStringLiteral("good-token"));
        CHECK(account.busy());
        account.signOut();  // before /me has answered
        CHECK_FALSE(account.busy());
        test::waitUntil([] { return false; }, 300);
        CHECK(account.state() == AccountController::SignedOut);
        CHECK(h.session.token().isEmpty());
        CHECK_FALSE(h.stored().has_value());
    }

    TEST_CASE("a slow startup read of the saved token does not override a newer sign-in")
    {
        Harness h("good-token");
        SlowSecretStore secrets;
        secrets.inner.write(QString::fromLatin1(kTokenKey), QStringLiteral("test"), "old-token", &secrets,
                            [](const Result<void>&) {});
        AccountController account(h.client, h.session, secrets);
        account.signInWithToken(QStringLiteral("good-token"));
        REQUIRE(test::waitUntil([&] { return account.signedIn(); }, 3000));

        secrets.release();  // the startup read finally answers with the token of an earlier session
        test::waitUntil([] { return false; }, 300);
        CHECK(h.session.token() == QByteArray("good-token"));
        CHECK(account.signedIn());
        CHECK_FALSE(account.busy());
    }

    TEST_CASE("a saved token that stopped working expires at startup, offering to sign in")
    {
        Harness h("good-token");
        h.preStore(QByteArray("dead-token"));
        AccountController account(h.client, h.session, h.secrets);
        QSignalSpy notice(&account, &AccountController::notice);
        REQUIRE(test::waitUntil([&] { return account.state() == AccountController::Expired; }, 3000));
        CHECK(h.session.token().isEmpty());
        CHECK_FALSE(h.stored().has_value());
        REQUIRE(test::waitUntil([&] { return !notice.isEmpty(); }));
        const auto arguments = notice.constLast();
        CHECK(arguments.at(0).toString() == u"warning");
        CHECK(arguments.at(3).toString() == u"open-accounts");

        // A refused attempt to sign in again keeps the session visibly expired.
        account.signInWithToken(QStringLiteral("wrong-token"));
        REQUIRE(test::waitUntil([&] { return !account.busy(); }, 3000));
        CHECK(account.state() == AccountController::Expired);
    }

    TEST_CASE("browser reader and token failures do not replace the session")
    {
        Harness h("good-token");
        FakeBrowserSource browsers;
        browsers.token = fail(ErrorCode::NotFound, "fixture has no cookie");
        AccountController account(h.client, h.session, h.secrets, &browsers);
        QSignalSpy notice(&account, &AccountController::notice);
        account.discoverBrowsers();
        REQUIRE(test::waitUntil([&] { return account.browserProfiles().size() == 1; }));
        account.importBrowserToken(QStringLiteral("profile-1"));
        REQUIRE(notice.wait(3000));
        CHECK(notice.constFirst().constFirst().toString() == u"error");
        CHECK(h.transport.count() == 0);
        CHECK(account.state() == AccountController::SignedOut);

        browsers.token = QByteArrayLiteral("wrong-token");
        account.importBrowserToken(QStringLiteral("profile-1"));
        REQUIRE(test::waitUntil([&] { return notice.count() >= 2; }, 3000));
        CHECK(account.state() == AccountController::SignedOut);
        CHECK(h.session.token().isEmpty());
        CHECK_FALSE(h.stored().has_value());
    }
}

TEST_SUITE("ui.catalog")
{
    TEST_CASE("signing in again after a sign-out loads the personal lists for the new account")
    {
        settings::SettingsStore store(settings::Schema::builtin());
        REQUIRE(store.setUserValue(settings::keys::ClientId, QString::fromLatin1(kClientId)));
        FakeApi transport;
        transport.setHandler([](const net::Request& request) -> Result<net::Response> {
            const QByteArray auth = FakeApi::header(request, QByteArrayLiteral("authorization"));
            if (request.url.path() == u"/me") {
                if (auth != "OAuth token-a" && auth != "OAuth token-b") {
                    return FakeApi::reply(401, R"({"error":"401 - Unauthorized"})", jsonHeaders());
                }
                const char* id = auth == "OAuth token-a" ? "1" : "2";
                return FakeApi::reply(200, QByteArray(R"({"id":)") + id + R"(,"username":"u","permalink":"u"})",
                                      jsonHeaders());
            }
            return FakeApi::reply(200, R"({"collection":[],"next_href":null})", jsonHeaders());
        });
        api::Session session;
        api::ApiService service(store, transport, session, {});
        auth::MemorySecretStore secrets;
        QTemporaryDir dir;
        REQUIRE(dir.isValid());
        api::SnapshotStore snapshots(dir.path());
        ui::AccountController account(service.client(), session, secrets);
        ui::CatalogController catalog(service, account, snapshots);
        const auto streamWith = [&](const QByteArray& token) {
            for (const net::Request& request : transport.requests()) {
                if (request.url.path() == u"/stream" &&
                    FakeApi::header(request, QByteArrayLiteral("authorization")) == "OAuth " + token) {
                    return true;
                }
            }
            return false;
        };

        account.signInWithToken(QStringLiteral("token-a"));
        REQUIRE(test::waitUntil([&] { return account.signedIn() && account.userId() == 1; }, 3000));
        catalog.ensure(QStringLiteral("feed"));  // Home shows the feed
        REQUIRE(test::waitUntil([&] { return streamWith("token-a"); }, 3000));

        account.signOut();
        account.signInWithToken(QStringLiteral("token-b"));
        REQUIRE(test::waitUntil([&] { return account.signedIn() && account.userId() == 2; }, 3000));
        catalog.ensure(QStringLiteral("feed"));  // Home asks again once signed in
        CHECK(test::waitUntil([&] { return streamWith("token-b"); }, 3000));
        CHECK(test::waitUntil([&] { return !catalog.feed()->loading(); }, 3000));
        CHECK(catalog.feed()->errorCode() == -1);
    }

    TEST_CASE("checking a candidate token keeps the saved lists of an unconfirmed saved session")
    {
        settings::SettingsStore store(settings::Schema::builtin());
        REQUIRE(store.setUserValue(settings::keys::ClientId, QString::fromLatin1(kClientId)));
        FakeApi transport;
        transport.setHandler([](const net::Request& request) -> Result<net::Response> {
            const QByteArray auth = FakeApi::header(request, QByteArrayLiteral("authorization"));
            if (auth == "OAuth saved-token") {
                return fail(ErrorCode::ConnectionFailed, "offline");  // the saved session stays unconfirmed
            }
            return FakeApi::reply(401, R"({"error":"401 - Unauthorized"})", jsonHeaders());
        });
        api::Session session;
        api::ApiService service(store, transport, session, {});
        auth::MemorySecretStore secrets;
        secrets.write(QString::fromLatin1(kTokenKey), QStringLiteral("test"), "saved-token", &secrets,
                      [](const Result<void>&) {});
        QTemporaryDir dir;
        REQUIRE(dir.isValid());
        // A previous run left account 42's saved likes.
        REQUIRE(QDir(dir.path()).mkpath(QStringLiteral("42")));
        QFile remembered(dir.filePath(QStringLiteral("account")));
        REQUIRE(remembered.open(QIODevice::WriteOnly));
        remembered.write("42");
        remembered.close();
        QFile likes(dir.filePath(QStringLiteral("42/likes.json")));
        REQUIRE(likes.open(QIODevice::WriteOnly));
        likes.write(R"({"collection":[]})");
        likes.close();
        api::SnapshotStore snapshots(dir.path());
        REQUIRE(snapshots.lastAccount() == 42);

        ui::AccountController account(service.client(), session, secrets);
        ui::CatalogController catalog(service, account, snapshots);
        QSignalSpy notice(&account, &ui::AccountController::notice);
        REQUIRE(test::waitUntil([&] { return account.signedIn(); }, 3000));
        catalog.ensure(QStringLiteral("feed"));  // the remembered account stands in for the unconfirmed one

        account.signInWithToken(QStringLiteral("wrong-token"));
        REQUIRE(notice.wait(3000));
        CHECK(account.signedIn());
        snapshots.flush();
        CHECK(QFile::exists(dir.filePath(QStringLiteral("42/likes.json"))));
        CHECK(snapshots.lastAccount() == 42);
    }
}

TEST_SUITE("ui.catalog-writes")
{
    // A signed-in account (id 1) with playlist 5 = [1, 2]; PUTs change it, so a later read sees the change.
    struct WriteHarness {
        settings::SettingsStore store{settings::Schema::builtin()};
        FakeApi transport;
        std::shared_ptr<std::mutex> lock = std::make_shared<std::mutex>();
        std::shared_ptr<std::vector<qint64>> playlist =
            std::make_shared<std::vector<qint64>>(std::vector<qint64>{1, 2});
        std::shared_ptr<int> likeStatus = std::make_shared<int>(200);
        api::Session session;
        std::unique_ptr<api::ApiService> service;
        auth::MemorySecretStore secrets;
        QTemporaryDir dir;
        std::unique_ptr<api::SnapshotStore> snapshots;
        std::unique_ptr<ui::AccountController> account;
        std::unique_ptr<ui::CatalogController> catalog;

        WriteHarness()
        {
            REQUIRE(store.setUserValue(settings::keys::ClientId, QString::fromLatin1(kClientId)));
            transport.setHandler([lock = lock, playlist = playlist,
                                  likeStatus = likeStatus](const net::Request& request) -> Result<net::Response> {
                const QString path = request.url.path();
                const std::scoped_lock guard(*lock);
                const auto playlistJson = [&] {
                    QJsonArray tracks;
                    for (const qint64 id : *playlist) {
                        tracks.append(
                            QJsonObject{{QStringLiteral("kind"), QStringLiteral("track")}, {QStringLiteral("id"), id}});
                    }
                    return QJsonDocument(
                               QJsonObject{{QStringLiteral("kind"), QStringLiteral("playlist")},
                                           {QStringLiteral("id"), 5},
                                           {QStringLiteral("title"), QStringLiteral("Mix")},
                                           {QStringLiteral("track_count"), static_cast<qint64>(playlist->size())},
                                           {QStringLiteral("tracks"), tracks}})
                        .toJson();
                };
                if (path == u"/me") {
                    return FakeApi::reply(200, R"({"id":1,"username":"me","permalink":"me"})", jsonHeaders());
                }
                if (path == u"/playlists/5" && request.method == net::Method::Get) {
                    return FakeApi::reply(200, playlistJson(), jsonHeaders());
                }
                if (path == u"/playlists/5" && request.method == net::Method::Put) {
                    playlist->clear();
                    for (const QJsonValue id :
                         QJsonDocument::fromJson(request.body)[u"playlist"][u"tracks"].toArray()) {
                        playlist->push_back(id.toInteger());
                    }
                    return FakeApi::reply(200, playlistJson(), jsonHeaders());
                }
                if (path == u"/playlists" && request.method == net::Method::Post) {
                    return FakeApi::reply(201, R"({"kind":"playlist","id":6,"title":"New one","tracks":[{"id":9}]})",
                                          jsonHeaders());
                }
                if (path.startsWith(u"/users/1/track_likes/")) {
                    return FakeApi::reply(*likeStatus, "{}", jsonHeaders());
                }
                if (path == u"/tracks/9") {
                    return FakeApi::reply(
                        200, R"({"kind":"track","id":9,"title":"Nine","user":{"id":3,"username":"a"}})", jsonHeaders());
                }
                return FakeApi::reply(200, R"({"collection":[],"next_href":null})", jsonHeaders());
            });
            service = std::make_unique<api::ApiService>(store, transport, session, QString());
            REQUIRE(dir.isValid());
            snapshots = std::make_unique<api::SnapshotStore>(dir.path());
            account = std::make_unique<ui::AccountController>(service->client(), session, secrets);
            catalog = std::make_unique<ui::CatalogController>(*service, *account, *snapshots);
            account->signInWithToken(QStringLiteral("token"));
            REQUIRE(test::waitUntil([&] { return account->signedIn() && account->userId() == 1; }, 3000));
        }
        [[nodiscard]] std::vector<qint64> tracks() const
        {
            const std::scoped_lock guard(*lock);
            return *playlist;
        }
        [[nodiscard]] int puts() const
        {
            return static_cast<int>(std::ranges::count_if(transport.requests(), [](const net::Request& r) {
                return r.method == net::Method::Put && r.url.path() == u"/playlists/5";
            }));
        }
    };

    TEST_CASE("adding to a playlist writes the list as it is now, once per track, one change at a time")
    {
        WriteHarness h;
        QSignalSpy notice(h.catalog.get(), &ui::CatalogController::notice);
        h.catalog->addToPlaylist(5, 9);
        h.catalog->addToPlaylist(5, 10);  // made while the first is in flight: must not lose it
        REQUIRE(test::waitUntil([&] { return notice.count() == 2; }, 5000));
        CHECK(h.tracks() == std::vector<qint64>{1, 2, 9, 10});
        CHECK(h.puts() == 2);
        CHECK(notice.at(0).at(0).toString() == u"success");
        // Already there: nothing is written.
        h.catalog->addToPlaylist(5, 2);
        REQUIRE(test::waitUntil([&] { return notice.count() == 3; }, 5000));
        CHECK(notice.at(2).at(0).toString() == u"info");
        CHECK(h.puts() == 2);
        // Every read before a write asked SoundCloud, not the HTTP cache.
        for (const net::Request& request : h.transport.requests()) {
            if (request.url.path() == u"/playlists/5" && request.method == net::Method::Get) {
                CHECK(request.cache == net::CacheMode::Refresh);
            }
        }
    }

    TEST_CASE("a like shows at once and goes back when SoundCloud refuses it")
    {
        WriteHarness h;
        QSignalSpy notice(h.catalog.get(), &ui::CatalogController::notice);
        CHECK_FALSE(h.catalog->isLiked(9));
        h.catalog->toggleLike(9);
        CHECK(h.catalog->isLiked(9));  // before any answer
        REQUIRE(test::waitUntil(
            [&] {
                return std::ranges::any_of(h.catalog->likes()->entries(),
                                           [](const api::Entry& entry) { return api::itemId(entry.item) == 9; });
            },
            5000));
        CHECK(h.catalog->isLiked(9));
        CHECK(notice.isEmpty());

        *h.likeStatus = 403;
        h.catalog->toggleLike(9);  // unlike, refused
        CHECK_FALSE(h.catalog->isLiked(9));
        REQUIRE(test::waitUntil([&] { return notice.count() == 1; }, 5000));
        CHECK(notice.at(0).at(0).toString() == u"error");
        CHECK(h.catalog->isLiked(9));
    }

    TEST_CASE("a new playlist is created with the track")
    {
        WriteHarness h;
        QSignalSpy notice(h.catalog.get(), &ui::CatalogController::notice);
        h.catalog->createPlaylist(QStringLiteral("  New one "), true, 9);
        REQUIRE(test::waitUntil([&] { return notice.count() == 1; }, 5000));
        CHECK(notice.at(0).at(0).toString() == u"success");
        const auto posts = std::ranges::count_if(h.transport.requests(), [](const net::Request& r) {
            return r.method == net::Method::Post && r.url.path() == u"/playlists";
        });
        REQUIRE(posts == 1);
        for (const net::Request& r : h.transport.requests()) {
            if (r.method == net::Method::Post) {
                const QJsonObject body = QJsonDocument::fromJson(r.body)[u"playlist"].toObject();
                CHECK(body[u"title"].toString() == u"New one");
                CHECK(body[u"sharing"].toString() == u"private");
                CHECK(body[u"tracks"].toArray() == QJsonArray{9});
            }
        }
        h.catalog->createPlaylist(QStringLiteral("   "), false, 9);  // a blank name sends nothing
        QTest::qWait(100);
        CHECK(notice.count() == 1);
    }
}

TEST_SUITE("ui.network-credentials")
{
    // A keyring whose startup read answers only on release(), with what it held when the read was made.
    class HeldReadSecretStore final : public QObject, public auth::ISecretStore {
    public:
        void read(const QString&, QObject* context, ReadCallback callback) override
        {
            m_held.emplace_back(QPointer(context), std::move(callback));
        }
        void write(const QString&, const QString&, const QByteArray&, QObject*, WriteCallback callback) override
        {
            callback({});
        }
        void remove(const QString&, QObject*, WriteCallback callback) override { callback({}); }
        [[nodiscard]] bool persistent() const noexcept override { return true; }
        [[nodiscard]] bool keyringBacked() const noexcept override { return true; }

        void release(const QByteArray& stored)
        {
            for (auto& [context, callback] : std::exchange(m_held, {})) {
                if (context != nullptr) {
                    callback(std::optional<QByteArray>(stored));
                }
            }
        }

    private:
        std::vector<std::pair<QPointer<QObject>, ReadCallback>> m_held;
    };

    struct NetworkHarness {
        QTemporaryDir dir;
        settings::SettingsStore store{settings::Schema::builtin()};
        settings::ConfigFile config{store, dir.filePath(QStringLiteral("config.toml")), {}};
        i18n::LanguageManager languages{store};
        ui::SettingsController settingsUi{store, config, languages};
        // Not System mode, so the controller starts no proxy lookup that could outlive the test.
        bool noProxy = store.setUserValue(settings::keys::ProxyMode, QStringLiteral("none")).has_value();
        net::NetworkService network{store, {}, false};
        HeldReadSecretStore secrets;
        ui::NetworkController controller{network, nullptr, secrets, languages, settingsUi};

        void releaseOldCredentials()
        {
            secrets.release(R"({"username":"old","password":"old"})");
            QCoreApplication::processEvents();
        }
    };

    TEST_CASE("a slow startup read does not bring back credentials saved meanwhile")
    {
        NetworkHarness h;
        REQUIRE(h.noProxy);
        h.controller.saveProxyCredentials(QStringLiteral("new"), QStringLiteral("pw"));
        h.releaseOldCredentials();
        CHECK(h.controller.proxyUsername() == u"new");
        CHECK(test::waitUntil([&] { return h.network.config().proxy.manual.username == u"new"; }, 2000));
        QTest::qWait(300);  // a late apply of the stale read would land here
        CHECK(h.network.config().proxy.manual.username == u"new");
    }

    TEST_CASE("a slow startup read does not bring back credentials forgotten meanwhile")
    {
        NetworkHarness h;
        h.controller.forgetProxyCredentials();
        h.releaseOldCredentials();
        CHECK_FALSE(h.controller.hasProxyCredentials());
        QTest::qWait(300);
        CHECK(h.network.config().proxy.manual.username.isEmpty());
    }

    TEST_CASE("the startup read applies stored credentials when nothing changed meanwhile")
    {
        NetworkHarness h;
        h.releaseOldCredentials();
        CHECK(h.controller.proxyUsername() == u"old");
        CHECK(test::waitUntil([&] { return h.network.config().proxy.manual.username == u"old"; }, 2000));
    }

    TEST_CASE("recent requests and the copied report show the resolver, address, size and attempt")
    {
        test::HttpServer origin([](const test::HttpRequest&) {
            return test::HttpReply{.status = 200, .headers = {}, .body = "twelve bytes"};
        });
        NetworkHarness h;
        REQUIRE(h.noProxy);
        h.releaseOldCredentials();
        QObject context;
        bool finished = false;
        net::Request request;
        // "%2F" in the path: a chained arg() would replace the "%2" with the next argument.
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/a%2Fb").arg(origin.port()), QUrl::StrictMode);
        h.network.transport().send(request, &context,
                                   {.onResponse = {}, .onData = {}, .onFinished = [&](auto) { finished = true; }});
        REQUIRE(test::waitUntil([&] { return finished && !h.controller.recentRequests().isEmpty(); }, 10000));

        const QVariantMap entry = h.controller.recentRequests().constFirst().toMap();
        CHECK(entry.value(QStringLiteral("dns")).toString() == ui::NetworkController::tr("IP address, no lookup"));
        CHECK(entry.value(QStringLiteral("address")).toString() == u"127.0.0.1");
        CHECK(entry.value(QStringLiteral("bytes")).toLongLong() == 12);
        CHECK(entry.value(QStringLiteral("attempt")).toInt() == 1);
        CHECK_FALSE(entry.value(QStringLiteral("internal")).toBool());

        const QString report = h.controller.report();
        INFO(report.toStdString());
        CHECK(report.contains(QStringLiteral("/a%2Fb → 200")));
        CHECK(report.contains(QStringLiteral("[dns literal, 127.0.0.1, 12 B]")));
    }
}
