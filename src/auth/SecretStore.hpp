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

private:
    void fallbackRead(const QString& key, QObject* context, ReadCallback callback);
    void fallbackWrite(const QString& key, const QString& label, const QByteArray& secret, quint64 epoch,
                       QObject* context, WriteCallback callback);
    void removeFromFallbacks(const QString& key);
    [[nodiscard]] bool superseded(const QString& key, quint64 epoch) const { return m_epochs.value(key) != epoch; }

    QThreadPool m_pool;
    std::unique_ptr<IKeyring> m_keyring;
    std::atomic<bool> m_available{true};
    std::atomic<bool> m_fileAvailable{false};
    std::unique_ptr<EncryptedFileSecretStore> m_file;
    MemorySecretStore m_fallback;
    // Bumped by every write and remove of a key: a slow write's fallback is dropped once a newer call exists.
    QHash<QString, quint64> m_epochs;
};

}  // namespace noctara::auth
