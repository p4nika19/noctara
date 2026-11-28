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
    SecretServiceKeyring() = default;
    ~SecretServiceKeyring() override
    {
        if (m_cancellable != nullptr) {
            g_object_unref(m_cancellable);
            m_cancellable = nullptr;
        }
    }
    SecretServiceKeyring(const SecretServiceKeyring&) = delete;
    SecretServiceKeyring& operator=(const SecretServiceKeyring&) = delete;
    SecretServiceKeyring(SecretServiceKeyring&&) = delete;
    SecretServiceKeyring& operator=(SecretServiceKeyring&&) = delete;

    Result<std::optional<QByteArray>> lookup(const QString& key) override
    {
        const QByteArray id = key.toUtf8();
        GError* error = nullptr;
        GCancellable* cancel = cancellable();
        gchar* value = secret_password_lookup_sync(schema(), cancel, &error, "key", id.constData(), nullptr);
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
        secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT, title.constData(), secret.constData(),
                                   cancellable(), &error, "key", id.constData(), nullptr);
        if (auto failure = takeError(error)) {
            return std::unexpected(*failure);
        }
        return {};
    }

    Result<void> clear(const QString& key) override
    {
        const QByteArray id = key.toUtf8();
        GError* error = nullptr;
        secret_password_clear_sync(schema(), cancellable(), &error, "key", id.constData(), nullptr);
        if (auto failure = takeError(error)) {
            return std::unexpected(*failure);
        }
        return {};
    }

    // A locked collection makes a lookup answer "no such secret" instead of failing, which is indistinguishable from
    // an account that never saved a session. Asking the collection costs a cheap local property read and needs no
    // prompt, so the answer can be told apart. Deliberately does not unlock: only an explicit user request may
    // raise a password dialog, and this runs while the app is still starting.
    [[nodiscard]] bool locked() override
    {
        GError* error = nullptr;
        SecretService* service = secret_service_get_sync(SECRET_SERVICE_NONE, cancellable(), &error);
        if (takeError(error) || service == nullptr) {
            return false;  // no Secret Service: the store reports that separately
        }
        SecretCollection* collection = secret_collection_for_alias_sync(service, SECRET_COLLECTION_DEFAULT,
                                                                        SecretCollectionFlags{}, cancellable(), &error);
        const bool isLocked = !takeError(error) && collection != nullptr && secret_collection_get_locked(collection);
        if (collection != nullptr) {
            g_object_unref(collection);
        }
        g_object_unref(service);
        return isLocked;
    }

    void cancel() override { g_cancellable_cancel(m_cancellable); }

    // Shows the desktop's unlock prompt (gcr-prompter under GNOME, the wallet dialog on Plasma). Only an explicit
    // user request may reach this: it blocks until the user answers or dismisses the dialog.
    [[nodiscard]] bool unlock() override
    {
        GError* error = nullptr;
        SecretService* service = secret_service_get_sync(SECRET_SERVICE_OPEN_SESSION, cancellable(), &error);
        if (takeError(error) || service == nullptr) {
            return false;
        }
        SecretCollection* collection = secret_collection_for_alias_sync(service, SECRET_COLLECTION_DEFAULT,
                                                                        SecretCollectionFlags{}, cancellable(), &error);
        bool unlocked = false;
        if (takeError(error) == std::nullopt && collection != nullptr) {
            GList* objects = g_list_append(nullptr, collection);
            GList* done = nullptr;
            const gint count = secret_service_unlock_sync(service, objects, cancellable(), &done, &error);
            (void)takeError(error);
            unlocked = count > 0 && secret_collection_get_locked(collection) == FALSE;
            if (done != nullptr) {
                g_list_free_full(done, g_object_unref);
            }
            g_list_free(objects);  // not owned: the collection is released below
            g_object_unref(collection);
        }
        g_object_unref(service);
        return unlocked;
    }

private:
    // Always pass m_cancellable — even once cancelled, so queued calls return G_IO_ERROR_CANCELLED immediately
    // instead of running uncancellable during shutdown.
    GCancellable* cancellable() const { return m_cancellable; }

    // Created with the keyring and never reset: a cancelled lookup answers Cancelled instead of blocking, which is
    // what lets Noctara quit while the desktop waits for an unlock prompt nobody will answer.
    GCancellable* m_cancellable = g_cancellable_new();
};

template <class T, class Callback> void deliver(QFuture<T> future, QObject* context, Callback callback)
{
    future.then(context, [callback = std::move(callback)](const T& result) { callback(result); });
}

void ignore(const Result<void>& /*result*/) {}

// One worker-thread round trip to the keyring: the answer, plus whether the collection was locked when it was given.
struct KeyringAnswer {
    Result<std::optional<QByteArray>> secret;
    bool locked = false;
};

struct KeyringWrite {
    Result<void> result;
    bool locked = false;
};

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
    // A keyring call may be waiting on an unlock prompt that will never be answered (a locked collection on a fresh
    // desktop session). Cancelling makes libsecret return instead of leaving quit hanging on waitForDone().
    if (m_keyring) {
        m_keyring->cancel();
    }
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

void SecretServiceStore::setLocked(bool locked)
{
    if (m_locked.exchange(locked) == locked) {
        return;
    }
    m_lockListeners.removeIf([](const LockListener& listener) { return listener.context.isNull(); });
    const QList<LockListener> listeners = m_lockListeners;
    for (const LockListener& listener : listeners) {
        if (listener.context != nullptr) {
            listener.callback(locked);
        }
    }
}

void SecretServiceStore::onLockStateChanged(QObject* context, LockCallback callback)
{
    if (context == nullptr || !callback) {
        return;
    }
    m_lockListeners.append({QPointer(context), std::move(callback)});
}

void SecretServiceStore::read(const QString& key, QObject* context, ReadCallback callback)
{
    if (!m_available) {
        fallbackRead(key, context, std::move(callback));
        return;
    }
    const quint64 unlockGen = m_unlockGeneration;
    deliver(
        QtConcurrent::run(&m_pool,
                          [keyring = m_keyring.get(), key] {
                              return KeyringAnswer{.secret = keyring->lookup(key), .locked = keyring->locked()};
                          }),
        this,
        [this, key, unlockGen, context = QPointer(context),
         callback = std::move(callback)](const KeyringAnswer& answer) {
            if (context == nullptr) {
                return;
            }
            const Result<std::optional<QByteArray>>& result = answer.secret;
            if (result && *result) {
                setLocked(false);
                const std::optional<QByteArray> keyringSecret = *result;
                if (!m_file) {
                    callback(keyringSecret);
                    return;
                }
                // A secret saved to the encrypted file while the keyring was unavailable is newer than the
                // keyring's older copy (every successful keyring write removes the file copy). Prefer it and
                // move it back into the keyring, so the next launch sees the same value in both places.
                const quint64 epoch = m_epochs.value(key);
                m_file->read(key, this,
                             [this, key, epoch, keyringSecret, context,
                              callback](Result<std::optional<QByteArray>> local) mutable {
                                 if (context == nullptr) {
                                     return;
                                 }
                                 if (!local || !*local) {
                                     callback(keyringSecret);  // no newer file copy (or it is unreadable)
                                     return;
                                 }
                                 if (m_epochs.value(key) != epoch) {
                                     callback(keyringSecret);  // rewritten or signed out meanwhile: do not resurrect
                                     return;
                                 }
                                 write(key, tr("Noctara saved secret"), **local, context,
                                       [callback, local](const Result<void>&) { callback(local); });
                             });
                return;
            }
            if (!result) {
                // No keyring on this system is an expected state, and the UI says so.
                qCInfo(lcAuth).noquote() << QString::fromStdString(result.error().describe())
                                         << "- using the local secret fallback";
                m_available = false;
                setLocked(false);
            } else if (answer.locked) {
                // The collection is locked, so "no such secret" means "not readable yet" rather than "never
                // saved". Keep the keyring and say so: the UI can offer to unlock it, and the next save must
                // still try the keyring instead of demoting every secret to the local file.
                // Do not clobber a newer successful unlock with a stale pre-unlock answer.
                if (m_unlockGeneration == unlockGen) {
                    setLocked(true);
                    qCInfo(lcAuth) << "the desktop keyring is locked; its secrets cannot be read until it is unlocked";
                }
            } else {
                setLocked(false);
            }
            // A keyring miss still checks the fallbacks: the secret may have been saved there in a session
            // without a keyring. The next write moves it into the keyring.
            fallbackRead(key, context, callback);
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
    deliver(QtConcurrent::run(&m_pool,
                              [keyring = m_keyring.get(), key, label, secret] -> KeyringWrite {
                                  // A locked collection cannot take a write: libsecret would open an unlock prompt and
                                  // block this worker thread until somebody answered it, which on a fresh desktop
                                  // session is nobody. Report it as a locked keyring instead, so the secret goes to the
                                  // local file and the user is told why.
                                  if (keyring->locked()) {
                                      return {.result = fail(ErrorCode::PermissionDenied,
                                                             "secret service: the keyring is locked"),
                                              .locked = true};
                                  }
                                  return {.result = keyring->store(key, label, secret), .locked = false};
                              }),
            this,
            [this, key, label, secret, epoch, context = QPointer(context),
             callback = std::move(callback)](const KeyringWrite& outcome) {
                if (outcome.result) {
                    setLocked(false);
                    removeFromFallbacks(key);  // an older copy there must not outlive this one
                    if (context != nullptr) {
                        callback({});
                    }
                    return;
                }
                qCWarning(lcAuth).noquote()
                    << QString::fromStdString(outcome.result.error().describe()) << "- using the local secret fallback";
                if (outcome.locked) {
                    // The keyring exists and is merely locked: keep it available so unlocking restores keyring use.
                    setLocked(true);
                } else {
                    m_available = false;
                    setLocked(false);
                }
                if (superseded(key, epoch)) {
                    // Removed (or rewritten) while this write was waiting on the keyring: do not bring it back.
                    if (context != nullptr) {
                        callback({});
                    }
                    return;
                }
                fallbackWrite(key, label, secret, epoch, context, callback);
            });
}

void SecretServiceStore::rereadAfterStartupUnlock(const QString& key, QObject* context, StartupReadCallback done,
                                                  bool unlockRefused)
{
    if (context == nullptr) {
        return;
    }
    // Read again after an unlock (or from fallbacks when a concurrent reader already saw the prompt dismissed).
    SecretServiceStore::read(key, context,
                             [done = std::move(done), unlockRefused](Result<std::optional<QByteArray>> secret) {
                                 done(std::move(secret), unlockRefused);
                             });
}  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks): libstdc++ std::function frees its target via _M_manager

void SecretServiceStore::finishStartupUnlock(bool unlocked)
{
    setLocked(!unlocked);
    m_startupUnlockRefused = !unlocked;
    m_unlockInFlight = false;
    QList<StartupWaiter> waiters = std::exchange(m_unlockWaiters, {});
    for (StartupWaiter& waiter : waiters) {
        QMetaObject::invokeMethod(
            this,
            [this, waiter = std::move(waiter), unlocked] mutable {
                rereadAfterStartupUnlock(waiter.key, waiter.context, std::move(waiter.done), !unlocked);
            },
            Qt::QueuedConnection);
    }
}

void SecretServiceStore::readAtStartup(const QString& key, QObject* context, StartupReadCallback done)
{
    const QPointer<QObject> guard(context);
    if (!m_unlockInFlight && m_startupUnlockRefused) {
        // The prompt was already shown and dismissed: do not raise it again for the second startup reader.
        rereadAfterStartupUnlock(key, guard, std::move(done), true);
        return;
    }

    const quint64 unlockGen = m_unlockGeneration;
    read(key, context,
         [this, key, guard, unlockGen, done = std::move(done)](Result<std::optional<QByteArray>> secret) mutable {
             if (guard == nullptr) {
                 return;
             }
             if (secret && *secret) {
                 // Already readable: deliver what we just read without a redundant second lookup.
                 done(std::move(secret), false);
                 return;
             }
             if (m_unlockGeneration != unlockGen) {
                 // Another reader unlocked the keyring while this initial read (or its fallback check) was in flight:
                 // the empty result predates the unlock, so read again now that the keyring is open.
                 rereadAfterStartupUnlock(key, guard, std::move(done), false);
                 return;
             }
             if (!keyringLocked()) {
                 // Not locked and no unlock happened meanwhile: deliver what we just read without a redundant lookup.
                 done(std::move(secret), false);
                 return;
             }
             if (m_unlockInFlight) {
                 // Another startup reader is already showing the desktop's prompt: wait for its answer.
                 m_unlockWaiters.append({.key = key, .context = guard, .done = std::move(done)});
                 return;
             }
             m_unlockInFlight = true;
             // Anchor on `this`, not `guard`: destroying the first caller must still clear m_unlockInFlight and wake
             // any other startup readers waiting in m_unlockWaiters.
             unlock(this, [this, key, guard, done = std::move(done)](const bool unlocked) mutable {
                 finishStartupUnlock(unlocked);
                 if (guard != nullptr) {
                     rereadAfterStartupUnlock(key, guard, std::move(done), !unlocked);
                 }
             });
         });
}

void SecretServiceStore::unlock(QObject* context, std::function<void(bool)> done)
{
    deliver(QtConcurrent::run(&m_pool,
                              [this] -> bool {
                                  if (!m_keyring->locked()) {
                                      return true;  // already readable (or not our kind of keyring)
                                  }
                                  return m_keyring->unlock();
                              }),
            this, [this, context = QPointer(context), done = std::move(done)](const bool& unlocked) {
                if (unlocked) {
                    ++m_unlockGeneration;
                    m_startupUnlockRefused = false;  // a later unlock clears the startup refusal
                    setLocked(false);
                }
                if (context != nullptr) {
                    done(unlocked);
                }
            });
}

void SecretServiceStore::remove(const QString& key, QObject* context, WriteCallback callback)
{
    ++m_epochs[key];
    removeFromFallbacks(key);
    // Always ask the keyring, even after it failed this session: a copy saved there in an earlier session would
    // otherwise come back at the next launch. Queued behind any pending keyring write for the same key.
    deliver(QtConcurrent::run(&m_pool,
                              [keyring = m_keyring.get(), key] {
                                  // Removing needs no unlock: a locked collection still deletes the item. Do not let it
                                  // block on a prompt that would show up for a mere sign-out.
                                  return keyring->clear(key);
                              }),
            this, [context = QPointer(context), callback = std::move(callback)](const Result<void>& result) {
                if (!result) {
                    qCWarning(lcAuth).noquote() << QString::fromStdString(result.error().describe())
                                                << "- could not remove a secret from the keyring";
                }
                if (context != nullptr) {
                    callback(result);
                }
            });
}

}  // namespace noctara::auth
