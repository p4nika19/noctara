#include "TestUtil.hpp"

#include "auth/EncryptedFileSecretStore.hpp"
#include "core/FileIo.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <memory>
#include <optional>

using namespace noctara;

namespace {

void writeSecret(auth::EncryptedFileSecretStore& store, const QString& key, const QByteArray& value)
{
    QObject context;
    auto result = std::make_shared<std::optional<Result<void>>>();
    store.write(key, QStringLiteral("Fixture"), value, &context,
                [result](const Result<void>& written) { result->emplace(written); });
    REQUIRE(test::waitUntil([&] { return result->has_value(); }));
    REQUIRE(**result);
}

Result<std::optional<QByteArray>> readSecret(auth::EncryptedFileSecretStore& store, const QString& key)
{
    QObject context;
    auto result = std::make_shared<std::optional<Result<std::optional<QByteArray>>>>();
    store.read(key, &context, [result](Result<std::optional<QByteArray>> value) { result->emplace(std::move(value)); });
    REQUIRE(test::waitUntil([&] { return result->has_value(); }));
    return std::move(**result);
}

void removeSecret(auth::EncryptedFileSecretStore& store, const QString& key)
{
    QObject context;
    auto result = std::make_shared<std::optional<Result<void>>>();
    store.remove(key, &context, [result](const Result<void>& removed) { result->emplace(removed); });
    REQUIRE(test::waitUntil([&] { return result->has_value(); }));
    REQUIRE(**result);
}

QString onlySecretFile(const QString& directory)
{
    const QStringList files = QDir(directory).entryList({QStringLiteral("*.secret")}, QDir::Files);
    REQUIRE(files.size() == 1);
    return directory + u'/' + files.constFirst();
}

}  // namespace

TEST_SUITE("auth.encrypted-secret-store")
{
    TEST_CASE("round-trips, overwrites, removes and persists across instances")
    {
        QTemporaryDir directory;
        const QString path = directory.path() + QStringLiteral("/secrets");
        {
            auth::EncryptedFileSecretStore store(path);
            CHECK(store.persistent());
            CHECK_FALSE(store.keyringBacked());
            writeSecret(store, QStringLiteral("soundcloud.oauth_token"), QByteArrayLiteral("first-secret"));
            writeSecret(store, QStringLiteral("soundcloud.oauth_token"), QByteArrayLiteral("second-secret"));
        }
        {
            auth::EncryptedFileSecretStore store(path);
            auto value = readSecret(store, QStringLiteral("soundcloud.oauth_token"));
            REQUIRE(value);
            REQUIRE(*value);
            CHECK(**value == QByteArrayLiteral("second-secret"));
            removeSecret(store, QStringLiteral("soundcloud.oauth_token"));
            auto missing = readSecret(store, QStringLiteral("soundcloud.oauth_token"));
            REQUIRE(missing);
            CHECK_FALSE(missing->has_value());
        }
    }

    TEST_CASE("uses owner-only files and never writes plaintext")
    {
        QTemporaryDir directory;
        const QString path = directory.path() + QStringLiteral("/secrets");
        auth::EncryptedFileSecretStore store(path);
        const QByteArray secret = QByteArrayLiteral("a-token-that-must-not-appear-in-the-file");
        writeSecret(store, QStringLiteral("soundcloud.oauth_token"), secret);

        QFile encrypted(onlySecretFile(path));
        REQUIRE(encrypted.open(QIODevice::ReadOnly));
        CHECK_FALSE(encrypted.readAll().contains(secret));
        const auto permissions = encrypted.permissions();
        CHECK((permissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
                              QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther)) ==
              QFileDevice::Permissions{});
        QFile key(path + QStringLiteral("/master.key"));
        REQUIRE(key.open(QIODevice::ReadOnly));
        CHECK(key.readAll().size() == 32);
        CHECK_FALSE(QFileInfo(path).permissions().testFlag(QFileDevice::ReadOther));
    }

    TEST_CASE("rejects tampering, a secret moved to another key and a wrong master key")
    {
        QTemporaryDir directory;
        const QString path = directory.path() + QStringLiteral("/secrets");
        auth::EncryptedFileSecretStore store(path);

        // Layout: 8-byte magic, 12-byte nonce, ciphertext, 16-byte tag. Flip one bit in each part after the magic.
        for (const qint64 offset : {qint64{12}, qint64{8 + 12}, qint64{-1}}) {
            CAPTURE(offset);
            writeSecret(store, QStringLiteral("key"), QByteArrayLiteral("secret"));
            QFile encrypted(onlySecretFile(path));
            REQUIRE(encrypted.open(QIODevice::ReadWrite));
            const qint64 position = offset < 0 ? encrypted.size() - 1 : offset;
            REQUIRE(encrypted.seek(position));
            QByteArray byte = encrypted.read(1);
            byte[0] = static_cast<char>(byte.at(0) ^ 0x01);
            REQUIRE(encrypted.seek(position));
            REQUIRE(encrypted.write(byte) == 1);
            encrypted.close();
            CHECK(test::errorOf(readSecret(store, QStringLiteral("key"))) == ErrorCode::Corrupt);
        }

        // The key name is authenticated data: a file copied under another key's name does not decrypt.
        writeSecret(store, QStringLiteral("key"), QByteArrayLiteral("secret"));
        const QString original = onlySecretFile(path);
        removeSecret(store, QStringLiteral("other"));
        writeSecret(store, QStringLiteral("other"), QByteArrayLiteral("placeholder"));
        const QStringList files = QDir(path).entryList({QStringLiteral("*.secret")}, QDir::Files);
        REQUIRE(files.size() == 2);
        const QString otherPath = path + u'/' + (path + u'/' + files.at(0) == original ? files.at(1) : files.at(0));
        REQUIRE(QFile::remove(otherPath));
        REQUIRE(QFile::copy(original, otherPath));
        CHECK(test::errorOf(readSecret(store, QStringLiteral("other"))) == ErrorCode::Corrupt);
        removeSecret(store, QStringLiteral("other"));

        writeSecret(store, QStringLiteral("key"), QByteArrayLiteral("new-secret"));
        REQUIRE(writeFileAtomic(path + QStringLiteral("/master.key"), QByteArray(32, 'x')));
        CHECK(test::errorOf(readSecret(store, QStringLiteral("key"))) == ErrorCode::Corrupt);
    }

    TEST_CASE("the master key is created once and never replaced")
    {
        QTemporaryDir directory;
        const QString path = directory.path() + QStringLiteral("/master.key");
        REQUIRE(createFileAtomic(path, QByteArray(32, 'a')));
        // A second creator (another Noctara instance starting at the same time) loses instead of replacing it.
        CHECK(test::errorOf(createFileAtomic(path, QByteArray(32, 'b'))) == ErrorCode::AlreadyExists);
        auto stored = readWholeFile(path);
        REQUIRE(stored);
        CHECK(*stored == QByteArray(32, 'a'));
        CHECK(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size() == 1);  // no temp file left
        CHECK_FALSE(QFileInfo(path).permissions().testFlag(QFileDevice::ReadOther));

        auth::EncryptedFileSecretStore store(directory.path());
        writeSecret(store, QStringLiteral("key"), QByteArrayLiteral("secret"));
        auto again = readWholeFile(path);
        REQUIRE(again);
        CHECK(*again == QByteArray(32, 'a'));  // the store used the existing key
    }
}
