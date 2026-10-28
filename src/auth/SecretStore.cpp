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

template <class T, class Callback> void deliver(QFuture<T> future, QObject* context, Callback callback)
{
    future.then(context, [callback = std::move(callback)](const T& result) { callback(result); });
}

}  // namespace

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
    : QObject(parent)
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
    if (m_file && m_fileAvailable) {
        m_file->read(key, context,
                     [this, key, context = QPointer(context),
                      callback = std::move(callback)](Result<std::optional<QByteArray>> result) mutable {
                         if (!result) {
                             m_fileAvailable = false;
                             if (context != nullptr) {
                                 m_fallback.read(key, context, std::move(callback));
                             }
                             return;
                         }
                         if (context != nullptr) {
                             callback(result);
                         }
                     });
        return;
    }
    m_fallback.read(key, context, std::move(callback));
}

void SecretServiceStore::fallbackWrite(const QString& key, const QString& label, const QByteArray& secret,
                                       QObject* context, WriteCallback callback)
{
    if (m_file && m_fileAvailable) {
        m_file->write(key, label, secret, context,
                      [this, key, label, secret, context = QPointer(context),
                       callback = std::move(callback)](const Result<void>& result) mutable {
                          if (!result) {
                              qCWarning(lcAuth).noquote()
                                  << QString::fromStdString(result.error().describe())
                                  << "- encrypted fallback unavailable; keeping secrets in memory only";
                              m_fileAvailable = false;
                              if (context != nullptr) {
                                  m_fallback.write(key, label, secret, context, std::move(callback));
                              }
                              return;
                          }
                          if (context != nullptr) {
                              callback(result);
                          }
                      });
        return;
    }
    m_fallback.write(key, label, secret, context, std::move(callback));
}

void SecretServiceStore::read(const QString& key, QObject* context, ReadCallback callback)
{
    if (!m_available) {
        fallbackRead(key, context, std::move(callback));
        return;
    }
    const QByteArray id = key.toUtf8();
    deliver(QtConcurrent::run(&m_pool,
                              [id] -> Result<std::optional<QByteArray>> {
                                  GError* error = nullptr;
                                  gchar* value = secret_password_lookup_sync(schema(), nullptr, &error, "key",
                                                                             id.constData(), nullptr);
                                  if (auto failure = takeError(error)) {
                                      return std::unexpected(*failure);
                                  }
                                  if (value == nullptr) {
                                      return std::nullopt;
                                  }
                                  QByteArray secret(value);
                                  secret_password_free(value);  // wipes the memory
                                  return secret;
                              }),
            this, [this, key, context = QPointer(context), callback](Result<std::optional<QByteArray>> result) {
                if (!result) {
                    qCWarning(lcAuth).noquote()
                        << QString::fromStdString(result.error().describe()) << "- using the local secret fallback";
                    m_available = false;
                    if (context != nullptr) {
                        fallbackRead(key, context, callback);
                    }
                    return;
                }
                if (context != nullptr) {
                    callback(result);
                }
            });
}

void SecretServiceStore::write(const QString& key, const QString& label, const QByteArray& secret, QObject* context,
                               WriteCallback callback)
{
    if (!m_available) {
        fallbackWrite(key, label, secret, context, std::move(callback));
        return;
    }
    const QByteArray id = key.toUtf8();
    const QByteArray title = label.toUtf8();
    deliver(QtConcurrent::run(&m_pool,
                              [id, title, secret] -> Result<void> {
                                  GError* error = nullptr;
                                  secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT, title.constData(),
                                                             secret.constData(), nullptr, &error, "key", id.constData(),
                                                             nullptr);
                                  if (auto failure = takeError(error)) {
                                      return std::unexpected(*failure);
                                  }
                                  return {};
                              }),
            this, [this, key, label, secret, context = QPointer(context), callback](const Result<void>& result) {
                if (!result) {
                    qCWarning(lcAuth).noquote()
                        << QString::fromStdString(result.error().describe()) << "- using the local secret fallback";
                    m_available = false;
                    if (context != nullptr) {
                        fallbackWrite(key, label, secret, context, callback);
                    }
                    return;
                }
                if (context != nullptr) {
                    callback(result);
                }
            });
}

void SecretServiceStore::remove(const QString& key, QObject* context, WriteCallback callback)
{
    // Best-effort removal from every tier the secret might have reached; a remove failure does not disable a
    // store (unlike a read/write failure), so the keyring stays primary for the rest of the session.
    m_fallback.remove(key, this, [](const Result<void>&) {});
    if (m_file) {
        m_file->remove(key, this, [](const Result<void>&) {});
    }
    if (!m_available) {
        QMetaObject::invokeMethod(context, [callback] { callback({}); }, Qt::QueuedConnection);
        return;
    }
    const QByteArray id = key.toUtf8();
    deliver(QtConcurrent::run(&m_pool,
                              [id] -> Result<void> {
                                  GError* error = nullptr;
                                  secret_password_clear_sync(schema(), nullptr, &error, "key", id.constData(), nullptr);
                                  if (auto failure = takeError(error)) {
                                      return std::unexpected(*failure);
                                  }
                                  return {};
                              }),
            this, [context = QPointer(context), callback](const Result<void>& result) {
                if (context != nullptr) {
                    callback(result);
                }
            });
}

}  // namespace noctara::auth
