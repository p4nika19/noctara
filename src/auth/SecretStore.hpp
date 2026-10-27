#pragma once

#include "core/Error.hpp"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QThreadPool>

#include <atomic>
#include <functional>
#include <optional>

namespace noctara::auth {

// Secrets (proxy credentials now; the SoundCloud session in slice (f)) never go to the config file. Results
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
    // False while secrets only live in memory (no keyring): they are forgotten when Noctara quits.
    [[nodiscard]] virtual bool persistent() const noexcept = 0;
};

// Process memory only; used by tests and as the fallback without a keyring.
class MemorySecretStore : public QObject, public ISecretStore {
    Q_OBJECT

public:
    using QObject::QObject;

    void read(const QString& key, QObject* context, ReadCallback callback) override;
    void write(const QString& key, const QString& label, const QByteArray& secret, QObject* context,
               WriteCallback callback) override;
    void remove(const QString& key, QObject* context, WriteCallback callback) override;
    [[nodiscard]] bool persistent() const noexcept override { return false; }

private:
    QHash<QString, QByteArray> m_secrets;
};

// The desktop keyring through the Secret Service API (libsecret: GNOME Keyring, KWallet, KeePassXC …).
// Calls run on a private worker thread because they may block on an unlock prompt. If the service is
// missing, secrets are kept in memory instead and persistent() turns false.
class SecretServiceStore : public QObject, public ISecretStore {
    Q_OBJECT

public:
    explicit SecretServiceStore(QObject* parent = nullptr);
    ~SecretServiceStore() override;
    SecretServiceStore(const SecretServiceStore&) = delete;
    SecretServiceStore& operator=(const SecretServiceStore&) = delete;
    SecretServiceStore(SecretServiceStore&&) = delete;
    SecretServiceStore& operator=(SecretServiceStore&&) = delete;

    void read(const QString& key, QObject* context, ReadCallback callback) override;
    void write(const QString& key, const QString& label, const QByteArray& secret, QObject* context,
               WriteCallback callback) override;
    void remove(const QString& key, QObject* context, WriteCallback callback) override;
    [[nodiscard]] bool persistent() const noexcept override { return m_available.load(); }

private:
    QThreadPool m_pool;
    std::atomic<bool> m_available{true};
    MemorySecretStore m_fallback;
};

}  // namespace noctara::auth
