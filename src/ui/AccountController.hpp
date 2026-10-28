#pragma once

#include "auth/BrowserCookieSource.hpp"
#include "net/Transport.hpp"

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>

namespace noctara::api {
class IApiClient;
class Session;
struct User;
}  // namespace noctara::api

namespace noctara::auth {
class ISecretStore;
}

namespace noctara::ui {

// QML "Account" singleton. Both manual and consented browser-cookie sign-in funnel through signInWithToken(),
// which verifies the token against /me before keeping it. Secrets live only in ISecretStore and api::Session.
class AccountController : public QObject {
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY changed)
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool sessionExpired READ sessionExpired NOTIFY changed)
    Q_PROPERTY(bool persistent READ persistent NOTIFY changed)
    Q_PROPERTY(bool keyringAvailable READ keyringAvailable NOTIFY changed)
    Q_PROPERTY(QVariantList browserProfiles READ browserProfiles NOTIFY browserProfilesChanged)
    Q_PROPERTY(bool browserBusy READ browserBusy NOTIFY browserBusyChanged)
    Q_PROPERTY(QString username READ username NOTIFY changed)
    Q_PROPERTY(QString displayName READ displayName NOTIFY changed)
    Q_PROPERTY(QString permalinkUrl READ permalinkUrl NOTIFY changed)
    Q_PROPERTY(QUrl avatarUrl READ avatarUrl NOTIFY changed)

public:
    enum State : int {
        SignedOut,
        Verifying,
        SignedIn,
        Expired,
    };
    Q_ENUM(State)

    AccountController(api::IApiClient& client, api::Session& session, auth::ISecretStore& secrets,
                      auth::IBrowserCookieSource* browsers = nullptr, QObject* parent = nullptr);

    [[nodiscard]] State state() const noexcept { return m_state; }
    [[nodiscard]] bool signedIn() const noexcept { return m_state == SignedIn; }
    [[nodiscard]] bool busy() const noexcept { return m_busy; }
    [[nodiscard]] bool sessionExpired() const noexcept { return m_state == Expired; }
    [[nodiscard]] bool persistent() const;
    [[nodiscard]] bool keyringAvailable() const;
    [[nodiscard]] QVariantList browserProfiles() const;
    [[nodiscard]] bool browserBusy() const noexcept { return m_browserBusy; }
    [[nodiscard]] const QString& username() const noexcept { return m_username; }
    [[nodiscard]] const QString& displayName() const noexcept { return m_displayName; }
    [[nodiscard]] const QString& permalinkUrl() const noexcept { return m_permalinkUrl; }
    [[nodiscard]] const QUrl& avatarUrl() const noexcept { return m_avatarUrl; }

    Q_INVOKABLE void signInWithToken(const QString& token);
    Q_INVOKABLE void signOut();
    Q_INVOKABLE void refresh();
    // Discovery reads profile metadata only. importBrowserToken() is called after explicit in-app consent.
    Q_INVOKABLE void discoverBrowsers();
    Q_INVOKABLE void importBrowserToken(const QString& profileId);

Q_SIGNALS:
    void changed();
    void busyChanged();
    void browserProfilesChanged();
    void browserBusyChanged();
    void notice(const QString& kind, const QString& message);

private:
    void loadStored();
    void verify(const QByteArray& token, bool interactive);
    void applyUser(const api::User& user);
    void clearAccount();
    void setState(State state);
    void setBusy(bool busy);
    void setBrowserBusy(bool busy);
    void onRejected();

    api::IApiClient& m_client;
    api::Session& m_session;
    auth::ISecretStore& m_secrets;
    auth::IBrowserCookieSource* m_browsers;
    QVector<auth::BrowserProfile> m_profiles;
    net::RequestHandle m_verify;
    int m_verifyGen = 0;
    bool m_verifying = false;
    State m_state = SignedOut;
    bool m_busy = false;
    bool m_browserBusy = false;
    QString m_username;
    QString m_displayName;
    QString m_permalinkUrl;
    QUrl m_avatarUrl;
};

}  // namespace noctara::ui
