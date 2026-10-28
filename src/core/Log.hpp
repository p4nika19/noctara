#pragma once

#include <QLoggingCategory>
#include <QString>
#include <QStringView>

Q_DECLARE_LOGGING_CATEGORY(lcCore)
Q_DECLARE_LOGGING_CATEGORY(lcSettings)
Q_DECLARE_LOGGING_CATEGORY(lcGfx)
Q_DECLARE_LOGGING_CATEGORY(lcTheme)
Q_DECLARE_LOGGING_CATEGORY(lcI18n)
Q_DECLARE_LOGGING_CATEGORY(lcUi)
Q_DECLARE_LOGGING_CATEGORY(lcNet)
Q_DECLARE_LOGGING_CATEGORY(lcCache)
Q_DECLARE_LOGGING_CATEGORY(lcApi)
Q_DECLARE_LOGGING_CATEGORY(lcMedia)
Q_DECLARE_LOGGING_CATEGORY(lcAuth)
Q_DECLARE_LOGGING_CATEGORY(lcDrm)

namespace noctara::log {

// Installs the process-wide message handler (timestamp, category, level).
void install();

// Number of warnings (and worse) logged since install(); used by the smoke test.
[[nodiscard]] int warningCount() noexcept;

// Placeholder for secret material in logs and diagnostics. Never reveals content or length.
[[nodiscard]] inline QString redacted()
{
    return QStringLiteral("<redacted>");
}

// Strips user-info (credentials) from a URL-like string: scheme://user:pass@host -> scheme://<redacted>@host.
[[nodiscard]] QString redactUrlCredentials(QStringView url);

}  // namespace noctara::log
