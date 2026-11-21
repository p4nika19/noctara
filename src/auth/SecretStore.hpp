#pragma once

#include "core/Error.hpp"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QThreadPool>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace noctara::auth {

class EncryptedFileSecretStore;

// Secrets (proxy credentials and the SoundCloud session) never go to the config file. Results
// arrive on `context`'s thread and are dropped if it is destroyed first.
class ISecretStore {
public:
    ISecretStore() = default;
    virtual ~ISecretStore() = default;
    ISecretStore(const ISecretStore&) = delete;
    ISecretStore& operator=(const ISecretStore&) = delete;
    ISecretStore(ISecretStore&&) = delete;
    ISecretStore& operator=(ISecretStore&&) = delete;

    using ReadCallback = std::function<void(Result<std::optional<QByteArray>>)>;
    using WriteCallback = std::function<void(Result<void>)>;

    virtual void read(const QString& key, QObject* context, ReadCallback callback) = 0;
    // `label` is what the user sees in their keyring manager.
    virtual void write(const QString& key, const QString& label, const QByteArray& secret, QObject* context,
                       WriteCallback callback) = 0;
    virtual void remove(const QString& key, QObject* context, WriteCallback callback) = 0;
    // False only when secrets are memory-only and forgotten when Noctara quits.
    [[nodiscard]] virtual bool persistent() const noexcept = 0;
    [[nodiscard]] virtual bool keyringBacked() const noexcept = 0;
    // The keyring exists but is locked, so a secret saved in it cannot be read until the user unlocks it. This is
    // why a signed-in account can look signed out right after logging in to the desktop: the keyring unlocks with
    // the session, and until then its secrets are not visible to anyone.
    [[nodiscard]] virtual bool keyringLocked() const noexcept { return false; }
    // Asks the desktop to unlock its keyring, which shows the desktop's own password prompt. `done` receives
    // whether the keyring is readable afterwards; callers reload their secret then. Stores without a keyring have
    // nothing to unlock and answer false.
    virtual void unlock(QObject* context, std::function<void(bool unlocked)> done)
    {
        QMetaObject::invokeMethod(context, [done = std::move(done)] { done(false); }, Qt::QueuedConnection);
    }
    // Startup read: read the secret, and if the keyring is locked show its own unlock prompt and read again. This
    // is the one path allowed to raise the prompt without a click — only once, at startup, so a fresh desktop
    // session with a locked keyring restores the saved session and proxy credentials before the user can notice.
    // `done` gets the secret (or nullopt) and whether the desktop refused/could not unlock the keyring.
    using StartupReadCallback = std::function<void(Result<std::optional<QByteArray>> secret, bool unlockRefused)>;
    virtual void readAtStartup(const QString& key, QObject* context, StartupReadCallback done)
    {
        read(key, context,
             [done = std::move(done)](Result<std::optional<QByteArray>> secret) { done(std::move(secret), false); });
    }
};

// Process memory only; used by tests and as the final fallback when no persistent store can be written.
class MemorySecretStore : public QObject, public ISecretStore {
    Q_OBJECT

public:
    using QObject::QObject;

    void read(const QString& key, QObject* context, ReadCallback callback) override;
    void write(const QString& key, const QString& label, const QByteArray& secret, QObject* context,
               WriteCallback callback) override;
    void remove(const QString& key, QObject* context, WriteCallback callback) override;
    [[nodiscard]] bool persistent() const noexcept override { return false; }
    [[nodiscard]] bool keyringBacked() const noexcept override { return false; }

private:
    QHash<QString, QByteArray> m_secrets;
};

// Blocking access to a desktop keyring. SecretServiceStore calls it only from its worker thread, one call at a time.
class IKeyring {
public:
    IKeyring() = default;
    virtual ~IKeyring() = default;
    IKeyring(const IKeyring&) = delete;
    IKeyring& operator=(const IKeyring&) = delete;
    IKeyring(IKeyring&&) = delete;
    IKeyring& operator=(IKeyring&&) = delete;

    // An error means the keyring itself is unusable (missing service, locked and not unlocked …).
    virtual Result<std::optional<QByteArray>> lookup(const QString& key) = 0;
    virtual Result<void> store(const QString& key, const QString& label, const QByteArray& secret) = 0;
    virtual Result<void> clear(const QString& key) = 0;
    // Whether the collection holding this application's secrets cannot be read right now because it is locked.
    // A locked collection answers a lookup with "no such secret" rather than an error, and only an unlock prompt
    // can tell the user apart from someone who never saved anything, so it must be asked before the answer counts.
    // False when there is no keyring or it cannot tell; implementations may report a definite "no" cheaply.
    [[nodiscard]] virtual bool locked() { return false; }
    // Cancels a lookup, store or clear that is waiting on the desktop (an unlock prompt nobody answered). Called
    // from another thread, for example when Noctara is quitting and must not hang on the worker.
    virtual void cancel() {}
    // Asks the desktop to unlock the collection, showing its own prompt. Blocking; only for an explicit user
    // request. Returns whether the collection is readable afterwards.
    [[nodiscard]] virtual bool unlock() { return true; }
};

// The Secret Service API through libsecret (GNOME Keyring, KWallet, KeePassXC …).
std::unique_ptr<IKeyring> makeSecretServiceKeyring();

// The desktop keyring, with EncryptedFileSecretStore (when a directory is supplied) and then memory as fallbacks.
// Keyring calls run on a private worker thread because they may block on an unlock prompt.
// - read: the keyring; on a miss or a keyring error, the encrypted file, then memory, so a secret saved while the
//   keyring was unavailable is still found when it comes back.
// - write: the first tier that accepts it; copies in lower tiers are then removed so they cannot shadow it later.
// - remove: every tier, the keyring included even after it failed, and a write still in flight cannot bring the
//   secret back through a fallback afterwards.
// After a keyring error, reads and writes skip the keyring for the rest of the session (no repeated prompts).
class SecretServiceStore : public QObject, public ISecretStore {
    Q_OBJECT

public:
    explicit SecretServiceStore(QObject* parent = nullptr);
    explicit SecretServiceStore(QString encryptedDirectory, QObject* parent = nullptr);
    SecretServiceStore(std::unique_ptr<IKeyring> keyring, QString encryptedDirectory, QObject* parent = nullptr);
    ~SecretServiceStore() override;
    SecretServiceStore(const SecretServiceStore&) = delete;
    SecretServiceStore& operator=(const SecretServiceStore&) = delete;
    SecretServiceStore(SecretServiceStore&&) = delete;
    SecretServiceStore& operator=(SecretServiceStore&&) = delete;

    void read(const QString& key, QObject* context, ReadCallback callback) override;
    void write(const QString& key, const QString& label, const QByteArray& secret, QObject* context,
               WriteCallback callback) override;
    void remove(const QString& key, QObject* context, WriteCallback callback) override;
    [[nodiscard]] bool persistent() const noexcept override { return m_available.load() || m_fileAvailable.load(); }
    [[nodiscard]] bool keyringBacked() const noexcept override { return m_available.load(); }
    // The keyring exists but its secrets cannot be read until the user unlocks it. A saved session is then present
    // but unusable, which is not the same as "never saved" and must not be reported as one.
    [[nodiscard]] bool keyringLocked() const noexcept override { return m_locked.load(); }
    // Asks the desktop to unlock, which shows its own password prompt. Runs on the worker thread; `done` receives
    // whether the keyring is readable afterwards. Only call it for an explicit user request: a startup read must
    // never raise a dialog.
    void unlock(QObject* context, std::function<void(bool unlocked)> done) override;
    // Startup read: read once, and if the collection is locked, unlock it (with its prompt) and read again.
    void readAtStartup(const QString& key, QObject* context, StartupReadCallback done) override;

private:
    void fallbackRead(const QString& key, QObject* context, ReadCallback callback);
    void fallbackWrite(const QString& key, const QString& label, const QByteArray& secret, quint64 epoch,
                       QObject* context, WriteCallback callback);
    void removeFromFallbacks(const QString& key);
    [[nodiscard]] bool superseded(const QString& key, quint64 epoch) const { return m_epochs.value(key) != epoch; }
    // Single-flight for the startup unlock: the account and proxy controllers both read at startup, but the desktop
    // must show its password prompt only once. Later readers get the result of the first unlock.
    void finishStartupUnlock(bool unlocked, QObject* context);

    QThreadPool m_pool;
    std::unique_ptr<IKeyring> m_keyring;
    std::atomic<bool> m_available{true};
    std::atomic<bool> m_fileAvailable{false};
    std::atomic<bool> m_locked{false};
    bool m_unlockInFlight = false;
    bool m_startupUnlockRefused = false;
    QList<std::function<void(bool)>> m_unlockWaiters;
    std::unique_ptr<EncryptedFileSecretStore> m_file;
    MemorySecretStore m_fallback;
    // Bumped by every write and remove of a key: a slow write's fallback is dropped once a newer call exists.
    QHash<QString, quint64> m_epochs;
};

}  // namespace noctara::auth
