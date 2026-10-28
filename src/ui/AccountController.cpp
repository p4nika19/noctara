#include "ui/AccountController.hpp"

#include "api/ApiClient.hpp"
#include "api/Endpoints.hpp"
#include "api/Session.hpp"
#include "api/Types.hpp"

#include "auth/SecretStore.hpp"
#include "i18n/ErrorMessages.hpp"

#include <QVariantMap>

namespace noctara::ui {

namespace {

// Stable key in the secret store; the user sees `label` in their keyring manager.
constexpr auto kTokenKey = "soundcloud.oauth_token";

bool isAuthFailure(ErrorCode code) noexcept
{
    return code == ErrorCode::Unauthorized || code == ErrorCode::Forbidden || code == ErrorCode::SignInRequired;
}

}  // namespace

AccountController::AccountController(api::IApiClient& client, api::Session& session, auth::ISecretStore& secrets,
                                     auth::IBrowserCookieSource* browsers, QObject* parent)
    : QObject(parent)
    , m_client(client)
    , m_session(session)
    , m_secrets(secrets)
    , m_browsers(browsers)
{
    // A 401 on any request (not just /me) means the stored token stopped working: fall back to Expired so the
    // page asks the user to sign in again. verify() owns the outcome while it is itself checking a token.
    connect(&m_session, &api::Session::rejected, this, &AccountController::onRejected);
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
    for (const auto& profile : m_profiles) {
        if (profile.id != profileId) {
            continue;
        }
        setBrowserBusy(true);
        m_browsers->importToken(profile, this, [this](Result<QByteArray> result) {
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
            QByteArray token = std::move(*result);
            signInWithToken(QString::fromLatin1(token));
            token.fill('\0');
        });
        return;
    }
    Q_EMIT notice(QStringLiteral("error"), tr("The selected browser profile is no longer available. Scan again."));
}

void AccountController::loadStored()
{
    m_secrets.read(QString::fromLatin1(kTokenKey), this, [this](Result<std::optional<QByteArray>> stored) {
        if (!stored || !*stored || (*stored)->isEmpty()) {
            return;  // nothing saved: stay signed out
        }
        const QByteArray token = **stored;
        m_session.setToken(token);
        // A saved token is presumed good until SoundCloud says otherwise, so the app is usable at once; the
        // account details (and any expiry) follow from the background check.
        setState(SignedIn);
        Q_EMIT changed();
        verify(token, /*interactive=*/false);
    });
}

void AccountController::signInWithToken(const QString& token)
{
    const QByteArray trimmed = token.trimmed().toUtf8();
    if (trimmed.isEmpty()) {
        Q_EMIT notice(QStringLiteral("error"), tr("Paste your SoundCloud OAuth token to sign in."));
        return;
    }
    verify(trimmed, /*interactive=*/true);
}

void AccountController::refresh()
{
    const QByteArray token = m_session.token();
    if (token.isEmpty()) {
        return;
    }
    verify(token, /*interactive=*/true);
}

void AccountController::verify(const QByteArray& token, bool interactive)
{
    const QByteArray previous = m_session.token();
    m_verify.cancel();  // drop any check still in flight
    const int generation = ++m_verifyGen;
    m_verifying = true;
    m_session.setToken(token);  // /me reads the token from the session
    if (interactive) {
        setBusy(true);
        setState(Verifying);
    }
    m_verify = api::fetch(
        m_client, api::endpoints::me(), this,
        [this, generation, interactive, token, previous](Result<api::User> result) {
            if (generation != m_verifyGen) {
                return;  // a newer verify() or signOut() superseded this one
            }
            m_verifying = false;
            if (interactive) {
                setBusy(false);
            }
            if (result) {
                applyUser(*result);
                setState(SignedIn);
                Q_EMIT changed();
                if (interactive) {
                    m_secrets.write(QString::fromLatin1(kTokenKey), tr("Noctara SoundCloud session"), token, this,
                                    [this](const Result<void>& written) {
                                        Q_EMIT changed();
                                        if (!written || !m_secrets.persistent()) {
                                            Q_EMIT notice(QStringLiteral("warning"),
                                                          tr("Signed in, but no persistent secret store is available, "
                                                             "so you will need to sign in again after Noctara quits."));
                                        } else if (m_secrets.keyringBacked()) {
                                            Q_EMIT notice(QStringLiteral("success"),
                                                          tr("Signed in. Your session is saved in the keyring."));
                                        } else {
                                            Q_EMIT notice(QStringLiteral("warning"),
                                                          tr("Signed in. Your session is saved in Noctara's encrypted "
                                                             "local store because no desktop keyring is available."));
                                        }
                                    });
                }
                return;
            }
            const ErrorCode code = result.error().code;
            if (interactive) {
                // A rejected interactive sign-in must not disturb an existing session.
                m_session.setToken(previous);
                setState(previous.isEmpty() ? SignedOut : SignedIn);
                Q_EMIT changed();
                Q_EMIT notice(QStringLiteral("error"),
                              isAuthFailure(code)
                                  ? tr("That token was not accepted. Copy the whole oauth_token value and try again.")
                                  : i18n::errorMessage(code));
                return;
            }
            // Background check of a saved token.
            if (isAuthFailure(code)) {
                clearAccount();
                m_session.clear();
                m_secrets.remove(QString::fromLatin1(kTokenKey), this, [](const Result<void>&) {});
                setState(Expired);
                Q_EMIT changed();
                Q_EMIT notice(QStringLiteral("warning"),
                              tr("Your SoundCloud session has expired. Please sign in again."));
            }
            // A non-auth error (offline, timeout) leaves the saved token in place to retry later.
        });
}

void AccountController::signOut()
{
    m_verify.cancel();
    ++m_verifyGen;
    m_verifying = false;
    clearAccount();
    m_session.clear();
    m_secrets.remove(QString::fromLatin1(kTokenKey), this, [](const Result<void>&) {});
    setState(SignedOut);
    Q_EMIT changed();
    Q_EMIT notice(QStringLiteral("info"), tr("Signed out of SoundCloud."));
}

void AccountController::onRejected()
{
    if (m_verifying || m_session.token().isEmpty()) {
        return;  // verify() handles its own token; nothing to do once signed out
    }
    clearAccount();
    m_session.clear();
    m_secrets.remove(QString::fromLatin1(kTokenKey), this, [](const Result<void>&) {});
    setState(Expired);
    Q_EMIT changed();
    Q_EMIT notice(QStringLiteral("warning"), tr("Your SoundCloud session has expired. Please sign in again."));
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
