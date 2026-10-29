#pragma once

#include "api/Types.hpp"

#include "auth/BrowserCookieSource.hpp"
#include "net/Transport.hpp"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include <chrono>

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
//
// A candidate token is checked with its own /me request and only then installed in api::Session, so other
// requests never run with it and a bad one never disturbs a working session. Every sign-in, sign-out and expiry
// starts a new generation; answers that arrive for an older one (a slow keyring read at startup, a superseded
// check, a save finishing after sign-out) are dropped.
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
    // The signed-in user's SoundCloud id, 0 while nobody is signed in (or before /me answered).
    [[nodiscard]] api::Id userId() const noexcept { return m_userId; }
    [[nodiscard]] const QString& displayName() const noexcept { return m_displayName; }
    [[nodiscard]] const QString& permalinkUrl() const noexcept { return m_permalinkUrl; }
    [[nodiscard]] const QUrl& avatarUrl() const noexcept { return m_avatarUrl; }

    Q_INVOKABLE void signInWithToken(const QString& token);
    Q_INVOKABLE void signOut();
    // Re-checks the current session with SoundCloud (and expires it if the token stopped working).
    Q_INVOKABLE void refresh();
    // Discovery reads profile metadata only. importBrowserToken() is called after explicit in-app consent.
    Q_INVOKABLE void discoverBrowsers();
    Q_INVOKABLE void importBrowserToken(const QString& profileId);

Q_SIGNALS:
    void changed();
    void busyChanged();
    void browserProfilesChanged();
    void browserBusyChanged();
    // A toast; `actionId` is handled by App.triggerAction (e.g. "open-accounts").
    void notice(const QString& kind, const QString& message, const QString& actionLabel = {},
                const QString& actionId = {});

private:
    void loadStored();
    void verifyCandidate(const QByteArray& token);
    void checkSession();
    void expire();
    void saveToken(const QByteArray& token);
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
    net::RequestHandle m_verify;  // the interactive check of a candidate token
    net::RequestHandle m_check;   // the background check of the current session
    QTimer m_retry;               // repeats a background check that failed for a non-auth reason (offline …)
    std::chrono::milliseconds m_retryDelay;
    int m_generation = 0;
    State m_state = SignedOut;
    State m_beforeVerify = SignedOut;  // restored when a candidate token is refused
    bool m_busy = false;
    bool m_browserBusy = false;
    QString m_username;
    api::Id m_userId = 0;
    QString m_displayName;
    QString m_permalinkUrl;
    QUrl m_avatarUrl;
};

}  // namespace noctara::ui
