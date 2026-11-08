#include "auth/SecretStore.hpp"

#include "auth/EncryptedFileSecretStore.hpp"
#include "core/Log.hpp"

#include <QFuture>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

#include <libsecret/secret.h>

namespace noctara::auth {

namespace {

const SecretSchema* schema()
{
    static const SecretSchema instance = [] {
        SecretSchema value{};
        value.name = "io.noctara.Secret";
        value.flags = SECRET_SCHEMA_NONE;
        value.attributes[0] = {"key", SECRET_SCHEMA_ATTRIBUTE_STRING};
        return value;
    }();
    return &instance;
}

// Takes ownership of a GError and turns it into an error code; nullopt when there was none.
std::optional<Error> takeError(GError* error)
{
    if (error == nullptr) {
        return std::nullopt;
    }
    Error out{ErrorCode::Unsupported, std::string("secret service: ") + error->message};
    g_error_free(error);
    return out;
}

class SecretServiceKeyring final : public IKeyring {
public:
    Result<std::optional<QByteArray>> lookup(const QString& key) override
    {
        const QByteArray id = key.toUtf8();
        GError* error = nullptr;
        gchar* value = secret_password_lookup_sync(schema(), nullptr, &error, "key", id.constData(), nullptr);
        if (auto failure = takeError(error)) {
            return std::unexpected(*failure);
        }
        if (value == nullptr) {
            return std::nullopt;
        }
        QByteArray secret(value);
        secret_password_free(value);  // wipes the memory
        return secret;
    }

    Result<void> store(const QString& key, const QString& label, const QByteArray& secret) override
    {
        const QByteArray id = key.toUtf8();
        const QByteArray title = label.toUtf8();
        GError* error = nullptr;
        secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT, title.constData(), secret.constData(), nullptr,
                                   &error, "key", id.constData(), nullptr);
        if (auto failure = takeError(error)) {
            return std::unexpected(*failure);
        }
        return {};
    }

    Result<void> clear(const QString& key) override
    {
        const QByteArray id = key.toUtf8();
        GError* error = nullptr;
        secret_password_clear_sync(schema(), nullptr, &error, "key", id.constData(), nullptr);
        if (auto failure = takeError(error)) {
            return std::unexpected(*failure);
        }
        return {};
    }
};

template <class T, class Callback> void deliver(QFuture<T> future, QObject* context, Callback callback)
{
    future.then(context, [callback = std::move(callback)](T result) mutable { callback(std::move(result)); });
}

void ignore(const Result<void>& /*result*/) {}

}  // namespace

std::unique_ptr<IKeyring> makeSecretServiceKeyring()
{
    return std::make_unique<SecretServiceKeyring>();
}

void MemorySecretStore::read(const QString& key, QObject* context, ReadCallback callback)
{
    const Result<std::optional<QByteArray>> result =
        m_secrets.contains(key) ? std::optional(m_secrets.value(key)) : std::nullopt;
    QMetaObject::invokeMethod(context, [callback, result] { callback(result); }, Qt::QueuedConnection);
}

void MemorySecretStore::write(const QString& key, const QString& /*label*/, const QByteArray& secret, QObject* context,
                              WriteCallback callback)
{
    m_secrets.insert(key, secret);
    QMetaObject::invokeMethod(context, [callback] { callback({}); }, Qt::QueuedConnection);
}

void MemorySecretStore::remove(const QString& key, QObject* context, WriteCallback callback)
{
    m_secrets.remove(key);
    QMetaObject::invokeMethod(context, [callback] { callback({}); }, Qt::QueuedConnection);
}

SecretServiceStore::SecretServiceStore(QObject* parent)
    : SecretServiceStore(QString{}, parent)
{
}

SecretServiceStore::SecretServiceStore(QString encryptedDirectory, QObject* parent)
    : SecretServiceStore(makeSecretServiceKeyring(), std::move(encryptedDirectory), parent)
{
}

SecretServiceStore::SecretServiceStore(std::unique_ptr<IKeyring> keyring, QString encryptedDirectory, QObject* parent)
    : QObject(parent)
    , m_keyring(std::move(keyring))
{
    m_pool.setMaxThreadCount(1);  // keeps keyring operations in order
    if (!encryptedDirectory.isEmpty()) {
        m_file = std::make_unique<EncryptedFileSecretStore>(std::move(encryptedDirectory));
        m_fileAvailable = true;
    }
}

SecretServiceStore::~SecretServiceStore()
{
    m_pool.waitForDone();
}

void SecretServiceStore::fallbackRead(const QString& key, QObject* context, ReadCallback callback)
{
    // Memory first: it only holds secrets written after the file tier failed (or without one), and a successful
    // file write drops the memory copy, so a memory copy is always newer than the file.
    m_fallback.read(
        key, this,
        [this, key, context = QPointer(context),
         callback = std::move(callback)](Result<std::optional<QByteArray>> memory) mutable {
            if (context == nullptr) {
                return;
            }
            if (!m_file || (memory && *memory)) {
                callback(std::move(memory));
                return;
            }
            m_file->read(key, this,
                         [context, callback = std::move(callback)](Result<std::optional<QByteArray>> result) mutable {
                             if (context == nullptr) {
                                 return;
                             }
                             if (!result) {
                                 // One unreadable entry (tampered, written with another master key) says nothing
                                 // about the others, so the file tier stays enabled; only a failed write disables it.
                                 qCWarning(lcAuth).noquote() << QString::fromStdString(result.error().describe())
                                                             << "- ignoring the stored secret";
                                 result = std::nullopt;
                             }
                             callback(std::move(result));
                         });
        });
}

void SecretServiceStore::fallbackWrite(const QString& key, const QString& label, const QByteArray& secret,
                                       quint64 epoch, QObject* context, WriteCallback callback)
{
    if (!m_file || !m_fileAvailable) {
        m_fallback.write(key, label, secret, context, std::move(callback));
        return;
    }
    m_file->write(key, label, secret, this,
                  [this, key, label, secret, epoch, context = QPointer(context),
                   callback = std::move(callback)](const Result<void>& result) mutable {
                      if (result) {
                          m_fallback.remove(key, this, ignore);
                      } else {
                          qCWarning(lcAuth).noquote()
                              << QString::fromStdString(result.error().describe())
                              << "- encrypted fallback unavailable; keeping secrets in memory only";
                          m_fileAvailable = false;
                          if (!superseded(key, epoch)) {
                              m_fallback.write(key, label, secret, this, ignore);
                              // The older file copy would come back at the next launch although it was replaced;
                              // unlinking usually still works when writing does not (ENOSPC, EDQUOT).
                              m_file->remove(key, this, [](const Result<void>& removed) {
                                  if (!removed) {
                                      qCWarning(lcAuth).noquote()
                                          << QString::fromStdString(removed.error().describe())
                                          << "- could not remove the replaced secret from the encrypted fallback";
                                  }
                              });
                          }
                      }
                      if (context != nullptr) {
                          callback({});
                      }
                  });
}

void SecretServiceStore::removeFromFallbacks(const QString& key)
{
    m_fallback.remove(key, this, ignore);
    if (m_file) {
        m_file->remove(key, this, [](const Result<void>& result) {
            if (!result) {
                qCWarning(lcAuth).noquote() << QString::fromStdString(result.error().describe())
                                            << "- could not remove a secret from the encrypted fallback";
            }
        });
    }
}

void SecretServiceStore::read(const QString& key, QObject* context, ReadCallback callback)
{
    if (!m_available) {
        fallbackRead(key, context, std::move(callback));
        return;
    }
    deliver(QtConcurrent::run(&m_pool, [keyring = m_keyring.get(), key] { return keyring->lookup(key); }), this,
            [this, key, context = QPointer(context),
             callback = std::move(callback)](Result<std::optional<QByteArray>> result) mutable {
                if (context == nullptr) {
                    return;
                }
                if (result && *result) {
                    callback(std::move(result));
                    return;
                }
                if (!result) {
                    // No keyring on this system is an expected state, and the UI says so.
                    qCInfo(lcAuth).noquote()
                        << QString::fromStdString(result.error().describe()) << "- using the local secret fallback";
                    m_available = false;
                }
                // A keyring miss still checks the fallbacks: the secret may have been saved there in a session
                // without a keyring. The next write moves it into the keyring.
                fallbackRead(key, context, std::move(callback));
            });
}

void SecretServiceStore::write(const QString& key, const QString& label, const QByteArray& secret, QObject* context,
                               WriteCallback callback)
{
    const quint64 epoch = ++m_epochs[key];
    if (!m_available) {
        fallbackWrite(key, label, secret, epoch, context, std::move(callback));
        return;
    }
    deliver(QtConcurrent::run(&m_pool, [keyring = m_keyring.get(), key, label,
                                        secret] { return keyring->store(key, label, secret); }),
            this,
            [this, key, label, secret, epoch, context = QPointer(context),
             callback = std::move(callback)](const Result<void>& result) mutable {
                if (result) {
                    removeFromFallbacks(key);  // an older copy there must not outlive this one
                    if (context != nullptr) {
                        callback({});
                    }
                    return;
                }
                qCWarning(lcAuth).noquote()
                    << QString::fromStdString(result.error().describe()) << "- using the local secret fallback";
                m_available = false;
                if (superseded(key, epoch)) {
                    // Removed (or rewritten) while this write was waiting on the keyring: do not bring it back.
                    if (context != nullptr) {
                        callback({});
                    }
                    return;
                }
                fallbackWrite(key, label, secret, epoch, context, std::move(callback));
            });
}

void SecretServiceStore::remove(const QString& key, QObject* context, WriteCallback callback)
{
    ++m_epochs[key];
    removeFromFallbacks(key);
    // Always ask the keyring, even after it failed this session: a copy saved there in an earlier session would
    // otherwise come back at the next launch. Queued behind any pending keyring write for the same key.
    const bool expected = m_available;
    deliver(QtConcurrent::run(&m_pool, [keyring = m_keyring.get(), key] { return keyring->clear(key); }), this,
            [expected, context = QPointer(context), callback = std::move(callback)](const Result<void>& result) {
                if (!result && expected) {
                    qCWarning(lcAuth).noquote() << QString::fromStdString(result.error().describe())
                                                << "- could not remove a secret from the keyring";
                }
                if (context != nullptr) {
                    // Without a keyring there was nothing to remove there.
                    callback(expected ? result : Result<void>{});
                }
            });
}

}  // namespace noctara::auth
