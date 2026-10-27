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
    case ErrorCode::DohUnreachable:
        return QCoreApplication::translate("Errors", "The DNS-over-HTTPS server is unreachable.");
    case ErrorCode::DohInvalidResponse:
        return QCoreApplication::translate("Errors", "The DNS-over-HTTPS server returned an invalid answer.");
    case ErrorCode::ProxyConnectFailed:
        return QCoreApplication::translate("Errors", "Could not connect to the proxy server.");
    case ErrorCode::ProxyAuthRequired:
        return QCoreApplication::translate("Errors", "The proxy server requires a valid username and password.");
    case ErrorCode::TlsHandshakeFailed:
        return QCoreApplication::translate("Errors", "A secure connection could not be established.");
    case ErrorCode::ConnectionFailed: return QCoreApplication::translate("Errors", "Could not connect to the server.");
    case ErrorCode::ConnectionRefused:
        return QCoreApplication::translate("Errors", "The server refused the connection.");
    case ErrorCode::HttpError: return QCoreApplication::translate("Errors", "The server returned an error.");
    case ErrorCode::Offline: return QCoreApplication::translate("Errors", "You are offline.");
    case ErrorCode::RateLimited:
        return QCoreApplication::translate("Errors", "Too many requests. Please wait a moment.");
    case ErrorCode::Unauthorized:
        return QCoreApplication::translate("Errors", "Your session has expired. Please sign in again.");
    case ErrorCode::Forbidden: return QCoreApplication::translate("Errors", "Access to this content is not allowed.");
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
