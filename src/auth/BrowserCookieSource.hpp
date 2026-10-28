#pragma once

#include "core/Error.hpp"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QVector>

#include <cstdint>
#include <functional>

namespace noctara::auth {

enum class BrowserFamily : std::uint8_t {
    Chromium,
    Firefox,
};

// Contains only profile metadata. Cookie values never enter the discovery model.
struct BrowserProfile {
    QString id;
    QString browserName;
    QString profileName;
    BrowserFamily family = BrowserFamily::Chromium;
    QString databasePath;
    QString safeStorageApplication;
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

using ChromiumPasswordProvider = std::function<Result<QByteArray>(const BrowserProfile&)>;

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
