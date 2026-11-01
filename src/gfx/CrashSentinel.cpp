#include "gfx/CrashSentinel.hpp"

#include "core/FileIo.hpp"
#include "core/Log.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QQuickWindow>

namespace noctara::gfx {

CrashSentinel::CrashSentinel(QString path)
    : m_path(std::move(path))
    , m_foundAtStartup(QFile::exists(m_path))
{
    if (m_foundAtStartup) {
        qCWarning(lcGfx) << "graphics crash sentinel found: the previous launch did not reach its first frame";
    }
}

CrashSentinel::~CrashSentinel()
{
    disarm();
}

void CrashSentinel::arm()
{
    const QByteArray content = QStringLiteral("pid=%1 time=%2\n")
                                   .arg(QCoreApplication::applicationPid())
                                   .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate))
                                   .toUtf8();
    if (auto written = writeFileAtomic(m_path, content); !written) {
        qCWarning(lcGfx).noquote() << "could not write crash sentinel:"
                                   << QString::fromStdString(written.error().describe());
        return;
    }
    m_armed = true;
}

void CrashSentinel::disarm()
{
    if (m_armed || m_foundAtStartup) {
        QFile::remove(m_path);
        m_armed = false;
        m_foundAtStartup = false;
    }
}

void coverFirstFrame(CrashSentinel& sentinel, QQuickWindow& window, const QObject& context,
                     std::function<void()> firstFrame)
{
    if (!window.isVisible()) {
        sentinel.disarm();
        QObject::connect(
            &window, &QWindow::visibleChanged, &context,
            [&sentinel](bool visible) {
                if (visible) {
                    sentinel.arm();
                }
            },
            Qt::SingleShotConnection);
    }
    // frameSwapped comes from the render thread; the context's thread is the GUI thread, so the call is queued.
    QObject::connect(
        &window, &QQuickWindow::frameSwapped, &context,
        [&sentinel, firstFrame = std::move(firstFrame)] {
            sentinel.disarm();
            if (firstFrame) {
                firstFrame();
            }
        },
        Qt::SingleShotConnection);
}

}  // namespace noctara::gfx
