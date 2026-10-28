// The auth layer: the in-memory secret store's round-trip, and the Accounts view-model's sign-in by token
// (verified against /me before it is kept), expiry handling and sign-out, against tests/mock/FakeApi.hpp.
#include "TestUtil.hpp"
#include "api/ApiClient.hpp"
#include "api/ClientId.hpp"
#include "api/Endpoints.hpp"
#include "api/Session.hpp"
#include "mock/FakeApi.hpp"

#include "auth/SecretStore.hpp"
#include "ui/AccountController.hpp"

#include <QSignalSpy>

#include <optional>

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
