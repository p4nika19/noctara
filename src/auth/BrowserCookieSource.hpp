#pragma once

#include "core/Error.hpp"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QVector>

#include <cstdint>
#include <functional>
#include <optional>

namespace noctara::auth {

enum class BrowserFamily : std::uint8_t {
    Chromium,
    Firefox,
};

// How the browser is installed; the same browser can be present several times (native, Flatpak, Snap).
enum class BrowserInstall : std::uint8_t {
    Native,
    Flatpak,
    Snap,
};

// Contains only profile metadata. Cookie values never enter the discovery model.
struct BrowserProfile {
    QString id;
    QString browserName;
    QString profileName;  // empty when the browser keeps a single, unnamed profile (Opera, portable Firefox)
    BrowserFamily family = BrowserFamily::Chromium;
    BrowserInstall install = BrowserInstall::Native;
    QString databasePath;
    // Chromium family only: the name under which the browser keeps its cookie password in the OS secret store,
    // "<name> Safe Storage" (Secret Service label, KWallet entry in the "<name> Keys" folder).
    QString keyringName;
};

class IBrowserCookieSource {
public:
    IBrowserCookieSource() = default;
    virtual ~IBrowserCookieSource() = default;
    IBrowserCookieSource(const IBrowserCookieSource&) = delete;
    IBrowserCookieSource& operator=(const IBrowserCookieSource&) = delete;
    IBrowserCookieSource(IBrowserCookieSource&&) = delete;
    IBrowserCookieSource& operator=(IBrowserCookieSource&&) = delete;

    using DiscoverCallback = std::function<void(Result<QVector<BrowserProfile>>)>;
    using ImportCallback = std::function<void(Result<QByteArray>)>;

    virtual void discover(QObject* context, DiscoverCallback callback) = 0;
    virtual void importToken(const BrowserProfile& profile, QObject* context, ImportCallback callback) = 0;
};

// Returns the Chromium browser's "Safe Storage" password from the OS secret store, std::nullopt when the store holds
// none, or an error when a store holds it but cannot be unlocked. Asked only for "v11" cookies, and at most once
// per import, because it may show the desktop's unlock prompt.
using ChromiumPasswordProvider = std::function<Result<std::optional<QByteArray>>(const BrowserProfile&)>;

// The production provider: the Secret Service (libsecret), then KWallet over D-Bus (KWallet first on KDE Plasma).
Result<std::optional<QByteArray>> chromiumSafeStoragePassword(const BrowserProfile& profile);

// Linux browser discovery and cookie import. The home directory and Chromium password provider are injectable so
// tests never touch the user's actual browser profiles or keyring.
class BrowserCookieSource final : public QObject, public IBrowserCookieSource {
public:
    explicit BrowserCookieSource(QObject* parent = nullptr);
    BrowserCookieSource(QString homeDirectory, ChromiumPasswordProvider passwords, QObject* parent = nullptr);
    ~BrowserCookieSource() override;

    void discover(QObject* context, DiscoverCallback callback) override;
    void importToken(const BrowserProfile& profile, QObject* context, ImportCallback callback) override;

private:
    QString m_homeDirectory;
    QString m_configDirectory;
    ChromiumPasswordProvider m_passwords;
    QThreadPool m_pool;
};

}  // namespace noctara::auth
