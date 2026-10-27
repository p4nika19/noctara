#include "core/Log.hpp"

#include <QDateTime>

#include <cstdio>

Q_LOGGING_CATEGORY(lcCore, "noctara.core")
Q_LOGGING_CATEGORY(lcSettings, "noctara.settings")
Q_LOGGING_CATEGORY(lcGfx, "noctara.gfx")
Q_LOGGING_CATEGORY(lcTheme, "noctara.theme")
Q_LOGGING_CATEGORY(lcI18n, "noctara.i18n")
Q_LOGGING_CATEGORY(lcUi, "noctara.ui")
Q_LOGGING_CATEGORY(lcNet, "noctara.net")
Q_LOGGING_CATEGORY(lcCache, "noctara.cache")
Q_LOGGING_CATEGORY(lcMedia, "noctara.media")
Q_LOGGING_CATEGORY(lcAuth, "noctara.auth")
Q_LOGGING_CATEGORY(lcDrm, "noctara.drm")

namespace noctara::log {

namespace {

const char* levelTag(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg: return "debug";
    case QtInfoMsg: return "info ";
    case QtWarningMsg: return "warn ";
    case QtCriticalMsg: return "error";
    case QtFatalMsg: return "fatal";
    }
    return "?";
}

void handler(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    const QByteArray time = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")).toUtf8();
    const QByteArray text = message.toUtf8();
    std::fprintf(stderr, "%s %s [%s] %s\n", time.constData(), levelTag(type),
                 context.category != nullptr ? context.category : "default", text.constData());
    std::fflush(stderr);
}

}  // namespace

void install()
{
    qInstallMessageHandler(handler);
}

QString redactUrlCredentials(QStringView url)
{
    const qsizetype schemeEnd = url.indexOf(u"://");
    const qsizetype authorityStart = schemeEnd < 0 ? 0 : schemeEnd + 3;
    qsizetype authorityEnd = url.indexOf(u'/', authorityStart);
    if (authorityEnd < 0) {
        authorityEnd = url.size();
    }
    const qsizetype at = url.lastIndexOf(u'@', authorityEnd - 1);
    if (at < authorityStart) {
        return url.toString();
    }
    return url.left(authorityStart).toString() + redacted() + url.mid(at).toString();
}

}  // namespace noctara::log
