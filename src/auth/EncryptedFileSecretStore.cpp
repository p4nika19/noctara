#include "auth/EncryptedFileSecretStore.hpp"

#include "core/FileIo.hpp"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QtConcurrent/QtConcurrentRun>

#include <array>
#include <limits>
#include <memory>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <utility>

namespace noctara::auth {

namespace {

constexpr qsizetype kKeySize = 32;
constexpr qsizetype kNonceSize = 12;
constexpr qsizetype kTagSize = 16;
constexpr qsizetype kMaxSecretSize = 1024LL * 1024LL;
constexpr QByteArrayView kMagic("NOCTSEC1", 8);

struct CipherContextCloser {
    void operator()(EVP_CIPHER_CTX* context) const { EVP_CIPHER_CTX_free(context); }
};
using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, CipherContextCloser>;

struct SensitiveBytes : QByteArray {
    SensitiveBytes() = default;
    explicit SensitiveBytes(const QByteArray& other)
        : QByteArray(other)
    {
    }
    SensitiveBytes(qsizetype size, Qt::Initialization initialization)
        : QByteArray(size, initialization)
    {
    }
    SensitiveBytes(const SensitiveBytes&) = default;
    SensitiveBytes(SensitiveBytes&&) noexcept = default;
    SensitiveBytes& operator=(const SensitiveBytes&) = default;
    SensitiveBytes& operator=(SensitiveBytes&&) noexcept = default;
    ~SensitiveBytes()
    {
        if (!isEmpty()) {
            OPENSSL_cleanse(data(), static_cast<std::size_t>(size()));
        }
    }
};

QString secretPath(const QString& directory, const QString& key)
{
    return directory + u'/' +
           QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex()) +
           QStringLiteral(".secret");
}

Result<SensitiveBytes> masterKey(const QString& directory)
{
    if (auto created = ensureDirectory(directory); !created) {
        return std::unexpected(created.error());
    }
    const QString path = directory + QStringLiteral("/master.key");
    if (QFileInfo::exists(path)) {
        Result<QByteArray> stored = readWholeFile(path);
        if (!stored) {
            return std::unexpected(stored.error());
        }
        if (stored->size() != kKeySize) {
            return fail(ErrorCode::Corrupt, "Encrypted secret-store master key has the wrong size");
        }
        SensitiveBytes key(*stored);
        OPENSSL_cleanse(stored->data(), static_cast<std::size_t>(stored->size()));
        return key;
    }

    SensitiveBytes key(kKeySize, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(key.data()), static_cast<int>(key.size())) != 1) {
        return fail(ErrorCode::Unknown, "Could not generate the encrypted secret-store master key");
    }
    if (auto written = writeFileAtomic(path, key); !written) {
        return std::unexpected(written.error());
    }
    return key;
}

Result<QByteArray> encrypt(const QString& keyName, const QByteArray& secret, const QByteArray& key)
{
    if (secret.size() > kMaxSecretSize || keyName.toUtf8().size() > std::numeric_limits<int>::max()) {
        return fail(ErrorCode::InvalidArgument, "Secret is too large for the encrypted store");
    }
    std::array<unsigned char, kNonceSize> nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        return fail(ErrorCode::Unknown, "Could not generate a secret-store nonce");
    }
    const CipherContext context(EVP_CIPHER_CTX_new());
    if (!context) {
        return fail(ErrorCode::Unknown, "Could not allocate the secret-store cipher context");
    }
    QByteArray ciphertext(secret.size(), Qt::Uninitialized);
    int written = 0;
    int finalSize = 0;
    const QByteArray associated = keyName.toUtf8();
    std::array<unsigned char, kTagSize> tag{};
    const bool ok =
        EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_EncryptInit_ex(context.get(), nullptr, nullptr, reinterpret_cast<const unsigned char*>(key.constData()),
                           nonce.data()) == 1 &&
        EVP_EncryptUpdate(context.get(), nullptr, &written,
                          reinterpret_cast<const unsigned char*>(associated.constData()),
                          static_cast<int>(associated.size())) == 1 &&
        EVP_EncryptUpdate(context.get(), reinterpret_cast<unsigned char*>(ciphertext.data()), &written,
                          reinterpret_cast<const unsigned char*>(secret.constData()),
                          static_cast<int>(secret.size())) == 1 &&
        EVP_EncryptFinal_ex(context.get(), reinterpret_cast<unsigned char*>(ciphertext.data()) + written, &finalSize) ==
            1 &&
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()), tag.data()) == 1;
    if (!ok) {
        return fail(ErrorCode::Unknown, "Could not encrypt a secret");
    }
    ciphertext.truncate(written + finalSize);
    QByteArray result(kMagic);
    result.append(reinterpret_cast<const char*>(nonce.data()), static_cast<qsizetype>(nonce.size()));
    result.append(ciphertext);
    result.append(reinterpret_cast<const char*>(tag.data()), static_cast<qsizetype>(tag.size()));
    return result;
}

Result<QByteArray> decrypt(const QString& keyName, const QByteArray& encoded, const QByteArray& key)
{
    if (encoded.size() < kMagic.size() + kNonceSize + kTagSize || !encoded.startsWith(kMagic)) {
        return fail(ErrorCode::Corrupt, "Encrypted secret has an invalid header");
    }
    const auto* nonce = reinterpret_cast<const unsigned char*>(encoded.constData() + kMagic.size());
    const qsizetype ciphertextSize = encoded.size() - kMagic.size() - kNonceSize - kTagSize;
    const auto* ciphertext = reinterpret_cast<const unsigned char*>(encoded.constData() + kMagic.size() + kNonceSize);
    auto* tag = const_cast<unsigned char*>(
        reinterpret_cast<const unsigned char*>(encoded.constData() + encoded.size() - kTagSize));
    const QByteArray associated = keyName.toUtf8();
    if (ciphertextSize > std::numeric_limits<int>::max() || associated.size() > std::numeric_limits<int>::max()) {
        return fail(ErrorCode::InvalidArgument, "Encrypted secret is too large");
    }

    const CipherContext context(EVP_CIPHER_CTX_new());
    if (!context) {
        return fail(ErrorCode::Unknown, "Could not allocate the secret-store cipher context");
    }
    SensitiveBytes plaintext(static_cast<int>(ciphertextSize), Qt::Uninitialized);
    int read = 0;
    int finalSize = 0;
    const bool ok =
        EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_DecryptInit_ex(context.get(), nullptr, nullptr, reinterpret_cast<const unsigned char*>(key.constData()),
                           nonce) == 1 &&
        EVP_DecryptUpdate(context.get(), nullptr, &read, reinterpret_cast<const unsigned char*>(associated.constData()),
                          static_cast<int>(associated.size())) == 1 &&
        EVP_DecryptUpdate(context.get(), reinterpret_cast<unsigned char*>(plaintext.data()), &read, ciphertext,
                          static_cast<int>(ciphertextSize)) == 1 &&
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTagSize), tag) == 1 &&
        EVP_DecryptFinal_ex(context.get(), reinterpret_cast<unsigned char*>(plaintext.data()) + read, &finalSize) == 1;
    if (!ok) {
        return fail(ErrorCode::Corrupt, "Encrypted secret failed authentication");
    }
    plaintext.truncate(read + finalSize);
    return QByteArray(plaintext);
}

template <class T, class Callback> void deliver(QFuture<T> future, QObject* context, Callback callback)
{
    future.then(context, [callback = std::move(callback)](const T& result) { callback(result); });
}

}  // namespace

EncryptedFileSecretStore::EncryptedFileSecretStore(QString directory, QObject* parent)
    : QObject(parent)
    , m_directory(std::move(directory))
{
    m_pool.setMaxThreadCount(1);
}

EncryptedFileSecretStore::~EncryptedFileSecretStore()
{
    m_pool.waitForDone();
}

void EncryptedFileSecretStore::read(const QString& key, QObject* context, ReadCallback callback)
{
    deliver(QtConcurrent::run(&m_pool,
                              [directory = m_directory, key] -> Result<std::optional<QByteArray>> {
                                  auto master = masterKey(directory);
                                  if (!master) {
                                      return std::unexpected(master.error());
                                  }
                                  const QString path = secretPath(directory, key);
                                  if (!QFileInfo::exists(path)) {
                                      return std::nullopt;
                                  }
                                  auto encoded = readWholeFile(path);
                                  if (!encoded) {
                                      return std::unexpected(encoded.error());
                                  }
                                  auto plain = decrypt(key, *encoded, *master);
                                  if (!plain) {
                                      return std::unexpected(plain.error());
                                  }
                                  return std::optional(std::move(*plain));
                              }),
            context, std::move(callback));
}

void EncryptedFileSecretStore::write(const QString& key, const QString& /*label*/, const QByteArray& secret,
                                     QObject* context, WriteCallback callback)
{
    deliver(QtConcurrent::run(&m_pool,
                              [directory = m_directory, key, secret] -> Result<void> {
                                  auto master = masterKey(directory);
                                  if (!master) {
                                      return std::unexpected(master.error());
                                  }
                                  auto encoded = encrypt(key, secret, *master);
                                  if (!encoded) {
                                      return std::unexpected(encoded.error());
                                  }
                                  return writeFileAtomic(secretPath(directory, key), *encoded);
                              }),
            context, std::move(callback));
}

void EncryptedFileSecretStore::remove(const QString& key, QObject* context, WriteCallback callback)
{
    deliver(QtConcurrent::run(&m_pool,
                              [directory = m_directory, key] -> Result<void> {
                                  const QString path = secretPath(directory, key);
                                  if (!QFileInfo::exists(path)) {
                                      return {};
                                  }
                                  if (!QFile::remove(path)) {
                                      return fail(ErrorCode::IoError, "Could not remove an encrypted secret");
                                  }
                                  return {};
                              }),
            context, std::move(callback));
}

}  // namespace noctara::auth
