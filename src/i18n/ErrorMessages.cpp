#include "i18n/ErrorMessages.hpp"

#include <QCoreApplication>
#include <QFileInfo>

namespace noctara::i18n {

QString errorMessage(ErrorCode code)
{
    switch (code) {
    case ErrorCode::Unknown: return QCoreApplication::translate("Errors", "Something went wrong.");
    case ErrorCode::Cancelled: return QCoreApplication::translate("Errors", "The operation was cancelled.");
    case ErrorCode::InvalidArgument: return QCoreApplication::translate("Errors", "The request was invalid.");
    case ErrorCode::NotFound: return QCoreApplication::translate("Errors", "The item could not be found.");
    case ErrorCode::AlreadyExists: return QCoreApplication::translate("Errors", "The item already exists.");
    case ErrorCode::PermissionDenied:
        return QCoreApplication::translate("Errors", "Permission denied. Check that the location is writable.");
    case ErrorCode::NoSpace: return QCoreApplication::translate("Errors", "There is not enough free disk space.");
    case ErrorCode::IoError: return QCoreApplication::translate("Errors", "A file could not be read or written.");
    case ErrorCode::Corrupt:
        return QCoreApplication::translate("Errors", "Stored data was damaged and has been discarded.");
    case ErrorCode::ParseError: return QCoreApplication::translate("Errors", "Received data could not be understood.");
    case ErrorCode::Unsupported: return QCoreApplication::translate("Errors", "This is not supported on your system.");
    case ErrorCode::Timeout: return QCoreApplication::translate("Errors", "The operation timed out.");
    case ErrorCode::ConfigSyntaxError:
        return QCoreApplication::translate("Errors", "The configuration file contains a syntax error.");
    case ErrorCode::ConfigTypeMismatch:
        return QCoreApplication::translate("Errors", "A setting has a value of the wrong type.");
    case ErrorCode::ConfigOutOfRange:
        return QCoreApplication::translate("Errors", "A setting is outside its allowed range.");
    case ErrorCode::ConfigUnknownEnumValue:
        return QCoreApplication::translate("Errors", "A setting has a value that is not one of the allowed options.");
    case ErrorCode::ConfigInvalidValue: return QCoreApplication::translate("Errors", "A setting has an invalid value.");
    case ErrorCode::ConfigUnknownKey:
        return QCoreApplication::translate("Errors", "The configuration contains an unknown key.");
    case ErrorCode::ConfigOverridden:
        return QCoreApplication::translate("Errors",
                                           "This setting is fixed by an environment variable or a command-line flag.");
    case ErrorCode::ConfigNewerVersion:
        return QCoreApplication::translate("Errors", "The configuration was written by a newer version of Noctara.");
    case ErrorCode::GraphicsInitFailed:
        return QCoreApplication::translate("Errors", "The graphics driver could not be initialised.");
    case ErrorCode::GraphicsDeviceLost: return QCoreApplication::translate("Errors", "The graphics device was lost.");
    case ErrorCode::DnsResolutionFailed:
        return QCoreApplication::translate("Errors", "The server name could not be resolved. Check your DNS settings.");
    case ErrorCode::DnsServerUnreachable:
        return QCoreApplication::translate("Errors", "None of the configured DNS servers answered.");
    case ErrorCode::DohUnreachable:
        return QCoreApplication::translate("Errors", "The DNS-over-HTTPS server is unreachable.");
    case ErrorCode::DohInvalidResponse:
        return QCoreApplication::translate("Errors", "The DNS-over-HTTPS server returned an invalid answer.");
    case ErrorCode::DohInsecureUrl:
        return QCoreApplication::translate(
            "Errors", "The DNS-over-HTTPS address must start with https://. Plain http:// is allowed only for "
                      "servers on this computer or the local network, with “Allow insecure DoH” turned on.");
    case ErrorCode::ProxyNotConfigured:
        return QCoreApplication::translate("Errors", "A manual proxy is selected, but no proxy host is set.");
    case ErrorCode::ProxyConnectFailed:
        return QCoreApplication::translate("Errors", "Could not connect to the proxy server.");
    case ErrorCode::ProxyAuthRequired:
        return QCoreApplication::translate("Errors", "The proxy server requires a valid username and password.");
    case ErrorCode::ProxyTlsFailed:
        return QCoreApplication::translate("Errors", "The proxy server did not accept an encrypted connection. If its "
                                                     "provider lists it as HTTP(S), choose HTTP as the proxy type: "
                                                     "HTTPS sites stay encrypted.");
    case ErrorCode::TlsHandshakeFailed:
        return QCoreApplication::translate("Errors", "A secure connection could not be established.");
    case ErrorCode::TlsCertificateInvalid:
        return QCoreApplication::translate(
            "Errors", "The server's certificate could not be verified. If your network inspects secure traffic, "
                      "set its CA bundle under Network → Security.");
    case ErrorCode::ConnectionFailed: return QCoreApplication::translate("Errors", "Could not connect to the server.");
    case ErrorCode::ConnectionRefused:
        return QCoreApplication::translate("Errors", "The server refused the connection.");
    case ErrorCode::HttpError: return QCoreApplication::translate("Errors", "The server returned an error.");
    case ErrorCode::Offline: return QCoreApplication::translate("Errors", "You are offline.");
    case ErrorCode::MeteredDeferred:
        return QCoreApplication::translate("Errors", "Skipped to save data on a metered connection.");
    case ErrorCode::RateLimited:
        return QCoreApplication::translate("Errors", "Too many requests. Please wait a moment.");
    case ErrorCode::Unauthorized:
        return QCoreApplication::translate("Errors", "Your session has expired. Please sign in again.");
    case ErrorCode::Forbidden: return QCoreApplication::translate("Errors", "Access to this content is not allowed.");
    case ErrorCode::NotCacheable:
        return QCoreApplication::translate("Errors", "Protected tracks are never stored on this computer.");
    case ErrorCode::CacheLocked:
        return QCoreApplication::translate(
            "Errors", "Another Noctara window is using the cache, so this one keeps its cache in memory only.");
    case ErrorCode::ContentChanged:
        return QCoreApplication::translate("Errors", "The track changed on the server and is being reloaded.");
    case ErrorCode::ClientIdUnavailable:
        return QCoreApplication::translate(
            "Errors", "Could not connect to SoundCloud: its web player changed. Try again later, or set a client ID "
                      "under Advanced → SoundCloud.");
    case ErrorCode::InvalidResponse:
        return QCoreApplication::translate("Errors", "SoundCloud sent a response Noctara does not understand.");
    case ErrorCode::SignInRequired:
        return QCoreApplication::translate("Errors", "Sign in to your SoundCloud account to see this.");
    case ErrorCode::NotPlayable:
        return QCoreApplication::translate("Errors", "This track cannot be played with your account.");
    case ErrorCode::DecodeFailed:
        return QCoreApplication::translate("Errors", "This track's audio could not be decoded.");
    case ErrorCode::AudioOutputFailed:
        return QCoreApplication::translate("Errors",
                                           "No sound device is available. Check your system's audio settings.");
    case ErrorCode::ProtectedPlayback:
        return QCoreApplication::translate(
            "Errors", "This track is protected. Turn on protected tracks in Settings → Playback to play it.");
    case ErrorCode::ProtectedPlayerMissing:
        return QCoreApplication::translate(
            "Errors", "The protected player is not installed, so protected tracks are skipped. See Settings → "
                      "Playback → Protected tracks.");
    case ErrorCode::ProtectedPlayerInstalling:
        return QCoreApplication::translate("Errors", "The protected player is being set up. Protected tracks play "
                                                     "once it is ready.");
    case ErrorCode::ProtectedPlayerFailed:
        return QCoreApplication::translate("Errors", "The protected player stopped working. Protected tracks are "
                                                     "skipped for now.");
    case ErrorCode::ProtectedNetworkUnsupported:
        return QCoreApplication::translate(
            "Errors", "The protected player cannot use your custom DNS servers without leaking requests, so "
                      "protected tracks are skipped. Allow the system resolver as a fallback, use DNS over HTTPS, "
                      "or use a proxy that resolves names.");
    case ErrorCode::LicenseDenied:
        return QCoreApplication::translate("Errors", "SoundCloud did not allow this protected track to play here.");
    }
    return QCoreApplication::translate("Errors", "Something went wrong.");
}

QString configIssueMessage(const settings::ConfigIssue& issue)
{
    QString base = errorMessage(issue.code);
    const QString file = QFileInfo(issue.file).fileName();
    if (issue.line > 0 && !issue.key.isEmpty()) {
        //: %1 = explanation, %2 = setting key, %3 = file name, %4 = line number
        return QCoreApplication::translate("Errors", "%1 (%2 in %3, line %4)")
            .arg(base, issue.key, file)
            .arg(issue.line);
    }
    if (issue.line > 0) {
        //: %1 = explanation, %2 = file name, %3 = line number
        return QCoreApplication::translate("Errors", "%1 (%2, line %3)").arg(base, file).arg(issue.line);
    }
    if (!issue.key.isEmpty()) {
        //: %1 = explanation, %2 = setting key or option
        return QCoreApplication::translate("Errors", "%1 (%2)").arg(base, issue.key);
    }
    return base;
}

}  // namespace noctara::i18n
