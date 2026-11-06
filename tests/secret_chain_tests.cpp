// SecretServiceStore's tiers (keyring → encrypted file → memory) against a fake keyring: fallback, recovery in a
// later session, removal from every tier, and a sign-out racing a slow keyring write.
#include "TestUtil.hpp"

#include "auth/EncryptedFileSecretStore.hpp"
#include "auth/SecretStore.hpp"

#include <QFile>
#include <QHash>
#include <QTemporaryDir>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <unistd.h>

using namespace noctara;

namespace {

// Shared between the test (main thread) and the store's worker thread.
struct KeyringState {
    std::mutex mutex;
    std::condition_variable released;
    QHash<QString, QByteArray> entries;
    bool failLookup = false;
    bool failStore = false;
    bool holdStores = false;  // store() waits until the test releases it
    int clears = 0;

    void release()
    {
        const std::scoped_lock lock(mutex);
        holdStores = false;
        released.notify_all();
    }
    std::optional<QByteArray> entry(const QString& key)
    {
        const std::scoped_lock lock(mutex);
        return entries.contains(key) ? std::optional(entries.value(key)) : std::nullopt;
    }
};

class FakeKeyring final : public auth::IKeyring {
public:
    explicit FakeKeyring(std::shared_ptr<KeyringState> state)
        : m_state(std::move(state))
    {
    }

    Result<std::optional<QByteArray>> lookup(const QString& key) override
    {
        const std::scoped_lock lock(m_state->mutex);
        if (m_state->failLookup) {
            return fail(ErrorCode::Unsupported, "fake keyring: locked");
        }
        return m_state->entries.contains(key) ? std::optional(m_state->entries.value(key)) : std::nullopt;
    }

    Result<void> store(const QString& key, const QString& /*label*/, const QByteArray& secret) override
    {
        std::unique_lock lock(m_state->mutex);
        m_state->released.wait(lock, [&] { return !m_state->holdStores; });
        if (m_state->failStore) {
            return fail(ErrorCode::Unsupported, "fake keyring: store refused");
        }
        m_state->entries.insert(key, secret);
        return {};
    }

    Result<void> clear(const QString& key) override
    {
        const std::scoped_lock lock(m_state->mutex);
        ++m_state->clears;
        m_state->entries.remove(key);
        return {};
    }

private:
    std::shared_ptr<KeyringState> m_state;
};

const QString kKey = QStringLiteral("soundcloud.oauth_token");

template <class Store> void write(Store& store, const QByteArray& value)
{
    QObject context;
    auto done = std::make_shared<bool>(false);
    store.write(kKey, QStringLiteral("Fixture"), value, &context, [done](const Result<void>&) { *done = true; });
    REQUIRE(test::waitUntil([&] { return *done; }));
}

template <class Store> std::optional<QByteArray> read(Store& store)
{
    QObject context;
    auto out = std::make_shared<std::optional<std::optional<QByteArray>>>();
    store.read(kKey, &context,
               [out](Result<std::optional<QByteArray>> value) { out->emplace(value ? *value : std::nullopt); });
    REQUIRE(test::waitUntil([&] { return out->has_value(); }));
    return **out;
}

template <class Store> void remove(Store& store)
{
    QObject context;
    auto done = std::make_shared<bool>(false);
    store.remove(kKey, &context, [done](const Result<void>&) { *done = true; });
    REQUIRE(test::waitUntil([&] { return *done; }));
}

// Waits for the store's queued clean-ups (removals from lower tiers) to settle.
void settle()
{
    test::waitUntil([] { return false; }, 100);
}

}  // namespace

TEST_SUITE("auth.secret-chain")
{
    TEST_CASE("a working keyring holds the secret and nothing is left in the fallbacks")
    {
        QTemporaryDir directory;
        auto keyring = std::make_shared<KeyringState>();
        auth::SecretServiceStore store(std::make_unique<FakeKeyring>(keyring), directory.path());
        write(store, "token-1");
        settle();
        CHECK(keyring->entry(kKey) == QByteArray("token-1"));
        CHECK(read(store) == QByteArray("token-1"));
        CHECK(store.keyringBacked());
        auth::EncryptedFileSecretStore file(directory.path());
        CHECK_FALSE(read(file).has_value());
    }

    TEST_CASE("a secret saved while the keyring failed is found once the keyring works again")
    {
        QTemporaryDir directory;
        auto keyring = std::make_shared<KeyringState>();
        {
            keyring->failStore = true;
            auth::SecretServiceStore store(std::make_unique<FakeKeyring>(keyring), directory.path());
            write(store, "file-token");
            CHECK_FALSE(store.keyringBacked());
            CHECK(store.persistent());
        }
        keyring->failStore = false;
        auth::SecretServiceStore nextSession(std::make_unique<FakeKeyring>(keyring), directory.path());
        CHECK(read(nextSession) == QByteArray("file-token"));  // keyring miss falls through to the file
        CHECK(nextSession.keyringBacked());

        // The next save goes to the keyring and removes the file copy, which can no longer shadow it.
        write(nextSession, "keyring-token");
        settle();
        CHECK(keyring->entry(kKey) == QByteArray("keyring-token"));
        auth::EncryptedFileSecretStore file(directory.path());
        CHECK_FALSE(read(file).has_value());
    }

    TEST_CASE("remove clears every tier, the keyring included after it failed this session")
    {
        QTemporaryDir directory;
        auto keyring = std::make_shared<KeyringState>();
        keyring->entries.insert(kKey, "from-an-earlier-session");
        keyring->failLookup = true;
        auth::SecretServiceStore store(std::make_unique<FakeKeyring>(keyring), directory.path());
        CHECK_FALSE(read(store).has_value());
        REQUIRE_FALSE(store.keyringBacked());
        write(store, "file-token");  // goes to the file now

        remove(store);
        settle();
        CHECK(keyring->clears == 1);
        CHECK_FALSE(keyring->entry(kKey).has_value());
        auth::EncryptedFileSecretStore file(directory.path());
        CHECK_FALSE(read(file).has_value());
        CHECK_FALSE(read(store).has_value());
    }

    TEST_CASE("a removal while a keyring write is pending is not undone by its fallback")
    {
        QTemporaryDir directory;
        auto keyring = std::make_shared<KeyringState>();
        keyring->holdStores = true;
        keyring->failStore = true;
        auth::SecretServiceStore store(std::make_unique<FakeKeyring>(keyring), directory.path());

        QObject context;
        auto written = std::make_shared<bool>(false);
        store.write(kKey, QStringLiteral("Fixture"), "late-token", &context,
                    [written](const Result<void>&) { *written = true; });
        auto removed = std::make_shared<bool>(false);
        store.remove(kKey, &context, [removed](const Result<void>&) { *removed = true; });
        keyring->release();  // the keyring now refuses the write; it must not land in the file
        REQUIRE(test::waitUntil([&] { return *written && *removed; }));
        settle();

        CHECK_FALSE(read(store).has_value());
        auth::EncryptedFileSecretStore file(directory.path());
        CHECK_FALSE(read(file).has_value());
    }

    TEST_CASE("a newer secret kept in memory after a failed file write is not shadowed by the older file")
    {
        if (::geteuid() == 0) {
            MESSAGE("permissions do not apply to root; skipped");
            return;
        }
        QTemporaryDir directory;
        auto keyring = std::make_shared<KeyringState>();
        keyring->failStore = true;
        keyring->failLookup = true;
        auth::SecretServiceStore store(std::make_unique<FakeKeyring>(keyring), directory.path());
        write(store, "old-token");  // goes to the file
        settle();

        const auto writable = QFile::permissions(directory.path());
        REQUIRE(QFile::setPermissions(directory.path(), QFile::ReadOwner | QFile::ExeOwner));
        write(store, "new-token");  // the file write fails: kept in memory
        settle();
        CHECK(read(store) == QByteArray("new-token"));
        REQUIRE(QFile::setPermissions(directory.path(), writable));
    }

    TEST_CASE("a failed file write removes the older file copy so it does not come back next session")
    {
        if (::geteuid() == 0) {
            MESSAGE("permissions do not apply to root; skipped");
            return;
        }
        QTemporaryDir directory;
        auto keyring = std::make_shared<KeyringState>();
        keyring->failStore = true;
        keyring->failLookup = true;
        {
            auth::SecretServiceStore store(std::make_unique<FakeKeyring>(keyring), directory.path());
            write(store, "old-token");  // goes to the file
            settle();

            // An unreadable master key makes the next file write fail while the old file can still be unlinked.
            const QString masterKey = directory.path() + QStringLiteral("/master.key");
            const auto readable = QFile::permissions(masterKey);
            REQUIRE(QFile::setPermissions(masterKey, QFileDevice::Permissions{}));
            write(store, "new-token");
            settle();
            CHECK_FALSE(store.persistent());
            CHECK(read(store) == QByteArray("new-token"));
            REQUIRE(QFile::setPermissions(masterKey, readable));
        }
        auth::SecretServiceStore nextSession(std::make_unique<FakeKeyring>(keyring), directory.path());
        CHECK_FALSE(read(nextSession).has_value());  // forgotten, as the user was told; never the replaced one
    }

    TEST_CASE("without any persistent tier the secret lives in memory only")
    {
        auto keyring = std::make_shared<KeyringState>();
        keyring->failStore = true;
        auth::SecretServiceStore store(std::make_unique<FakeKeyring>(keyring), QString{});
        write(store, "memory-token");
        CHECK_FALSE(store.persistent());
        CHECK(read(store) == QByteArray("memory-token"));
    }
}
