#pragma once

#include "auth/SecretStore.hpp"

namespace noctara::auth {

// Owner-only, versioned AES-256-GCM files for systems without Secret Service. The random master key is kept
// separately in the same private app directory; this protects files at rest but is not a desktop keyring.
class EncryptedFileSecretStore final : public QObject, public ISecretStore {
    Q_OBJECT

public:
    explicit EncryptedFileSecretStore(QString directory, QObject* parent = nullptr);
    ~EncryptedFileSecretStore() override;

    void read(const QString& key, QObject* context, ReadCallback callback) override;
    void write(const QString& key, const QString& label, const QByteArray& secret, QObject* context,
               WriteCallback callback) override;
    void remove(const QString& key, QObject* context, WriteCallback callback) override;
    [[nodiscard]] bool persistent() const noexcept override { return true; }
    [[nodiscard]] bool keyringBacked() const noexcept override { return false; }

private:
    QString m_directory;
    QThreadPool m_pool;
};

}  // namespace noctara::auth
