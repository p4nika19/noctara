#include "media/SessionState.hpp"

#include "core/FileIo.hpp"
#include "core/Log.hpp"

#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace noctara::media {

namespace {

QString repeatName(Repeat repeat)
{
    switch (repeat) {
    case Repeat::All: return QStringLiteral("all");
    case Repeat::One: return QStringLiteral("one");
    case Repeat::Off: break;
    }
    return QStringLiteral("off");
}

}  // namespace

QByteArray SessionState::toJson() const
{
    const QJsonObject object{{QStringLiteral("version"), 1},
                             {QStringLiteral("volume"), volume},
                             {QStringLiteral("muted"), muted},
                             {QStringLiteral("shuffle"), shuffle},
                             {QStringLiteral("repeat"), repeatName(repeat)}};
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

SessionState SessionState::fromJson(const QByteArray& json)
{
    SessionState state;
    const QJsonObject object = QJsonDocument::fromJson(json).object();
    if (object.value(QStringLiteral("version")).toInt() != 1) {
        return state;
    }
    state.volume = std::clamp(object.value(QStringLiteral("volume")).toDouble(state.volume), 0.0, 1.0);
    state.muted = object.value(QStringLiteral("muted")).toBool();
    state.shuffle = object.value(QStringLiteral("shuffle")).toBool();
    const QString repeat = object.value(QStringLiteral("repeat")).toString();
    state.repeat = repeat == u"all" ? Repeat::All : (repeat == u"one" ? Repeat::One : Repeat::Off);
    return state;
}

SessionStore::SessionStore(QString path, QObject* parent)
    : QObject(parent)
    , m_path(std::move(path))
{
    m_writer.setMaxThreadCount(1);  // saves land in order
    m_timer.setSingleShot(true);
    m_timer.setInterval(kDebounce);
    connect(&m_timer, &QTimer::timeout, this, &SessionStore::flush);
    m_saved = load();
    m_pending = m_saved;
}

SessionStore::~SessionStore()
{
    flush();
    m_writer.waitForDone();
}

SessionState SessionStore::load() const
{
    if (m_path.isEmpty()) {
        return {};
    }
    const Result<QByteArray> data = readWholeFile(m_path);
    return data ? SessionState::fromJson(*data) : SessionState{};
}

void SessionStore::save(const SessionState& state)
{
    m_pending = state;
    if (!m_path.isEmpty()) {
        m_timer.start();
    }
}

void SessionStore::flush()
{
    m_timer.stop();
    if (m_path.isEmpty() || m_pending == m_saved) {
        return;
    }
    m_saved = m_pending;
    m_writer.start([path = m_path, data = m_pending.toJson()] {
        Result<void> written = ensureDirectory(path.section(u'/', 0, -2));
        if (written) {
            written = writeFileAtomic(path, data, 0600, Durability::Atomic);
        }
        if (!written) {
            qCWarning(lcMedia).noquote() << "could not save the player state:"
                                         << QString::fromStdString(written.error().describe());
        }
    });
}

}  // namespace noctara::media
