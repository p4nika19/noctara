#include "ui/AccountController.hpp"

#include "api/ApiClient.hpp"
#include "api/Endpoints.hpp"
#include "api/Session.hpp"
#include "api/Types.hpp"

#include "auth/SecretStore.hpp"
#include "auth/Token.hpp"
#include "i18n/ErrorMessages.hpp"

#include <QVariantMap>

#include <algorithm>

namespace noctara::ui {

namespace {

using namespace std::chrono_literals;

// Stable key in the secret store; the user sees `label` in their keyring manager.
constexpr auto kTokenKey = "soundcloud.oauth_token";

// A background check that failed for a non-auth reason (offline, timeout, 5xx) is repeated, backing off.
constexpr std::chrono::milliseconds kFirstRetry = 30s;
constexpr std::chrono::milliseconds kMaxRetry = 10min;

}  // namespace

AccountController::AccountController(api::IApiClient& client, api::Session& session, auth::ISecretStore& secrets,
                                     auth::IBrowserCookieSource* browsers, QObject* parent)
    : QObject(parent)
    , m_client(client)
    , m_session(session)
    , m_secrets(secrets)
    , m_browsers(browsers)
    , m_retryDelay(kFirstRetry)
{
    // A 401 on any request (not just /me) means the stored token stopped working: fall back to Expired so the
    // page asks the user to sign in again. Candidate tokens never cause this (see api::ApiRequest).
    connect(&m_session, &api::Session::rejected, this, &AccountController::onRejected);
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &AccountController::checkSession);
    loadStored();
}

bool AccountController::persistent() const
{
    return m_secrets.persistent();
}

bool AccountController::keyringAvailable() const
{
    return m_secrets.keyringBacked();
}

QVariantList AccountController::browserProfiles() const
{
    QVariantList result;
    result.reserve(m_profiles.size());
    for (const auto& profile : m_profiles) {
        // Flatpak and Snap are product names and stay untranslated; they tell apart installs of one browser.
        QString browser = profile.browserName;
        if (profile.install == auth::BrowserInstall::Flatpak) {
            browser = tr("%1 (Flatpak)", "browser installed as a Flatpak").arg(browser);
        } else if (profile.install == auth::BrowserInstall::Snap) {
            browser = tr("%1 (Snap)", "browser installed as a Snap").arg(browser);
        }
        const QString label = profile.profileName.isEmpty()
                                  ? browser
                                  : tr("%1 — %2", "browser, then the profile name").arg(browser, profile.profileName);
        result.append(QVariantMap{{QStringLiteral("id"), profile.id},
                                  {QStringLiteral("browser"), browser},
                                  {QStringLiteral("profile"), profile.profileName},
                                  {QStringLiteral("label"), label},
                                  {QStringLiteral("encrypted"), profile.family == auth::BrowserFamily::Chromium}});
    }
    return result;
}

void AccountController::discoverBrowsers()
{
    if (m_browsers == nullptr || m_browserBusy) {
        return;
    }
    setBrowserBusy(true);
    m_browsers->discover(this, [this](Result<QVector<auth::BrowserProfile>> result) {
        setBrowserBusy(false);
        if (!result) {
            Q_EMIT notice(QStringLiteral("error"), tr("Could not inspect the installed browser profiles."));
            return;
        }
        m_profiles = std::move(*result);
        Q_EMIT browserProfilesChanged();
        if (m_profiles.isEmpty()) {
            Q_EMIT notice(QStringLiteral("info"),
                          tr("No supported browser profiles were found. You can still paste your OAuth token."));
        }
    });
}

void AccountController::importBrowserToken(const QString& profileId)
{
    if (m_browsers == nullptr || m_browserBusy) {
        return;
    }
    const auto profile =
        std::ranges::find_if(m_profiles, [&](const auth::BrowserProfile& entry) { return entry.id == profileId; });
    if (profile == m_profiles.cend()) {
        Q_EMIT notice(QStringLiteral("error"), tr("The selected browser profile is no longer available. Scan again."));
        return;
    }
    setBrowserBusy(true);
    m_browsers->importToken(*profile, this, [this](Result<QByteArray> result) {
        setBrowserBusy(false);
        if (!result) {
            QString message;
            switch (result.error().code) {
            case ErrorCode::NotFound:
                message = tr("No SoundCloud session was found in this browser profile. Sign in to SoundCloud in "
                             "that browser first, or paste your OAuth token.");
                break;
            case ErrorCode::PermissionDenied:
                message = tr("Noctara could not unlock this browser's cookies. Unlock your desktop keyring and "
                             "try again, or paste your OAuth token.");
                break;
            case ErrorCode::Unsupported:
                message = tr("This browser's cookie encryption is not supported yet. Paste your OAuth token "
                             "instead.");
                break;
            default:
                message = tr("Could not read the selected browser profile. You can still paste your OAuth token.");
                break;
            }
            Q_EMIT notice(QStringLiteral("error"), message);
            return;
        }
        verifyCandidate(*result);
    });
}

void AccountController::loadStored()
{
    const int generation = m_generation;
    m_secrets.read(QString::fromLatin1(kTokenKey), this, [this, generation](Result<std::optional<QByteArray>> stored) {
        Q_EMIT changed();  // the store knows its protection level after its first answer
        if (generation != m_generation) {
            return;  // the user signed in or out while the keyring was still answering
        }
        if (!stored || !*stored || !auth::isPlausibleToken(**stored)) {
            return;  // nothing usable saved: stay signed out
        }
        // A saved token is presumed good until SoundCloud says otherwise, so the app is usable at
        // once; the account details (and any expiry) follow from the background check.
        m_session.setToken(**stored);
        setState(SignedIn);
        Q_EMIT changed();
        checkSession();
    });
}

void AccountController::signInWithToken(const QString& token)
{
    const QByteArray trimmed = token.trimmed().toUtf8();
    if (trimmed.isEmpty()) {
        Q_EMIT notice(QStringLiteral("error"), tr("Paste your SoundCloud OAuth token to sign in."));
        return;
    }
    if (!auth::isPlausibleToken(trimmed)) {
        Q_EMIT notice(QStringLiteral("error"),
                      tr("That does not look like an OAuth token. Copy only the oauth_token value, without spaces."));
        return;
    }
    verifyCandidate(trimmed);
}

void AccountController::refresh()
{
    checkSession();
}

void AccountController::verifyCandidate(const QByteArray& token)
{
    m_verify.cancel();
    m_check.cancel();
    m_retry.stop();
    const int generation = ++m_generation;
    if (m_state != Verifying) {
        m_beforeVerify = m_state;  // a check that supersedes another one keeps the original state to return to
    }
    setBusy(true);
    setState(Verifying);
    // The session keeps its current token (if any) until the candidate has been accepted.
    m_verify = api::fetch(m_client, api::endpoints::verifyToken(token), this,
                          [this, generation, token](Result<api::User> result) {
                              if (generation != m_generation) {
                                  return;  // a newer sign-in, a sign-out or an expiry superseded this check
                              }
                              setBusy(false);
                              if (result) {
                                  m_session.setToken(token);
                                  applyUser(*result);
                                  setState(SignedIn);
                                  saveToken(token);
                                  return;
                              }
                              setState(m_beforeVerify);  // an expired session stays visibly expired
                              const ErrorCode code = result.error().code;
                              Q_EMIT notice(QStringLiteral("error"),
                                            code == ErrorCode::Unauthorized || code == ErrorCode::Forbidden
                                                ? tr("That token was not accepted. Copy the whole oauth_token value "
                                                     "and try again.")
                                                : i18n::errorMessage(code));
                          });
}

void AccountController::saveToken(const QByteArray& token)
{
    const int generation = m_generation;
    m_secrets.write(QString::fromLatin1(kTokenKey), tr("Noctara SoundCloud session"), token, this,
                    [this, generation](const Result<void>& written) {
                        Q_EMIT changed();  // the protection level may have changed with this write
                        if (generation != m_generation) {
                            return;  // signed out meanwhile: no "saved" message
                        }
                        if (!written || !m_secrets.persistent()) {
                            Q_EMIT notice(QStringLiteral("warning"),
                                          tr("Signed in, but no persistent secret store is available, so you will "
                                             "need to sign in again after Noctara quits."));
                        } else if (m_secrets.keyringBacked()) {
                            Q_EMIT notice(QStringLiteral("success"),
                                          tr("Signed in. Your session is saved in the keyring."));
                        } else {
                            Q_EMIT notice(QStringLiteral("warning"),
                                          tr("Signed in. Your session is saved in Noctara's encrypted local store "
                                             "because no desktop keyring is available."));
                        }
                    });
}

void AccountController::checkSession()
{
    const QByteArray token = m_session.token();
    if (token.isEmpty() || m_state == Verifying) {
        return;
    }
    m_check.cancel();
    m_retry.stop();
    const int generation = m_generation;
    m_check = api::fetch(m_client, api::endpoints::me(), this, [this, generation, token](Result<api::User> result) {
        if (generation != m_generation || m_session.token() != token) {
            return;
        }
        if (result) {
            m_retryDelay = kFirstRetry;
            applyUser(*result);
            Q_EMIT changed();
            return;
        }
        if (result.error().code == ErrorCode::Unauthorized) {
            expire();
            return;
        }
        // Offline, timeout, a server error …: the saved token stays; ask again later.
        m_retry.start(m_retryDelay);
        m_retryDelay = std::min(m_retryDelay * 2, kMaxRetry);
    });
}

void AccountController::expire()
{
    if (m_session.token().isEmpty()) {
        return;
    }
    ++m_generation;
    m_verify.cancel();
    m_check.cancel();
    m_retry.stop();
    setBusy(false);
    clearAccount();
    m_session.clear();
    m_secrets.remove(QString::fromLatin1(kTokenKey), this, [this](const Result<void>&) { Q_EMIT changed(); });
    setState(Expired);
    Q_EMIT notice(QStringLiteral("warning"), tr("Your SoundCloud session has expired. Please sign in again."),
                  tr("Sign in"), QStringLiteral("open-accounts"));
}

void AccountController::signOut()
{
    ++m_generation;
    m_verify.cancel();
    m_check.cancel();
    m_retry.stop();
    setBusy(false);
    clearAccount();
    m_session.clear();
    m_secrets.remove(QString::fromLatin1(kTokenKey), this, [this](const Result<void>&) { Q_EMIT changed(); });
    setState(SignedOut);
    Q_EMIT notice(QStringLiteral("info"), tr("Signed out of SoundCloud."));
}

void AccountController::onRejected()
{
    expire();  // a no-op once signed out or already expired
}

void AccountController::applyUser(const api::User& user)
{
    m_username = user.username;
    m_displayName = user.fullName.isEmpty() ? user.username : user.fullName;
    m_permalinkUrl = user.permalinkUrl;
    m_avatarUrl = QUrl(user.avatarUrl);
}

void AccountController::clearAccount()
{
    m_username.clear();
    m_displayName.clear();
    m_permalinkUrl.clear();
    m_avatarUrl.clear();
}

void AccountController::setState(State state)
{
    if (m_state != state) {
        m_state = state;
        Q_EMIT changed();
    }
}

void AccountController::setBusy(bool busy)
{
    if (m_busy != busy) {
        m_busy = busy;
        Q_EMIT busyChanged();
    }
}

void AccountController::setBrowserBusy(bool busy)
{
    if (m_browserBusy != busy) {
        m_browserBusy = busy;
        Q_EMIT browserBusyChanged();
    }
}

}  // namespace noctara::ui
