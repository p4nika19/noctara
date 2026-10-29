#include "ui/GlobalShortcuts.hpp"

#include "core/Log.hpp"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

namespace noctara::ui {

QDBusArgument& operator<<(QDBusArgument& argument, const PortalShortcut& shortcut)
{
    argument.beginStructure();
    argument << shortcut.id << shortcut.properties;
    argument.endStructure();
    return argument;
}

// The signature Qt D-Bus requires for demarshalling (it returns the stream it was given).
const QDBusArgument& operator>>(const QDBusArgument& argument, PortalShortcut& shortcut)
{
    argument.beginStructure();
    argument >> shortcut.id >> shortcut.properties;
    argument.endStructure();
    return argument;  // NOLINT(bugprone-return-const-ref-from-parameter)
}

namespace {

constexpr QLatin1StringView kService("org.freedesktop.portal.Desktop");
constexpr QLatin1StringView kPath("/org/freedesktop/portal/desktop");
constexpr QLatin1StringView kInterface("org.freedesktop.portal.GlobalShortcuts");
constexpr QLatin1StringView kRequest("org.freedesktop.portal.Request");

}  // namespace

GlobalShortcuts::GlobalShortcuts(QObject* parent)
    : QObject(parent)
{
    qDBusRegisterMetaType<PortalShortcut>();
    qDBusRegisterMetaType<QList<PortalShortcut>>();
}

GlobalShortcuts::~GlobalShortcuts()
{
    closeSession();
}

bool GlobalShortcuts::available()
{
    const QDBusConnection bus = QDBusConnection::sessionBus();
    return bus.isConnected() && bus.interface() != nullptr && bus.interface()->isServiceRegistered(kService);
}

QString GlobalShortcuts::requestPath(const QString& token) const
{
    // The portal answers on /org/freedesktop/portal/desktop/request/<sender>/<token>, sender without ':' and
    // with '.' as '_'.
    QString sender = QDBusConnection::sessionBus().baseService().mid(1);
    sender.replace(u'.', u'_');
    return kPath + QStringLiteral("/request/") + sender + u'/' + token;
}

void GlobalShortcuts::setEnabled(bool enabled, std::vector<Action> actions)
{
    m_enabled = enabled;
    m_actions = std::move(actions);
    if (!enabled) {
        closeSession();
        return;
    }
    if (!m_session.isEmpty() || !m_pending.isEmpty() || !available()) {
        return;
    }
    QDBusConnection bus = QDBusConnection::sessionBus();
    const QString token = QStringLiteral("noctara%1").arg(++m_counter);
    m_pending = requestPath(token);
    bus.connect(kService, m_pending, kRequest, QStringLiteral("Response"), this,
                SLOT(onSessionCreated(uint, QVariantMap)));
    QDBusMessage call = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("CreateSession"));
    call << QVariantMap{{QStringLiteral("handle_token"), token},
                        {QStringLiteral("session_handle_token"), QStringLiteral("noctara_session%1").arg(m_counter)}};
    (void)bus.asyncCall(call);
}

void GlobalShortcuts::onSessionCreated(uint response, const QVariantMap& results)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.disconnect(kService, m_pending, kRequest, QStringLiteral("Response"), this,
                   SLOT(onSessionCreated(uint, QVariantMap)));
    m_pending.clear();
    if (response != 0) {
        qCInfo(lcUi) << "global shortcuts: the portal refused a session (" << response << ")";
        return;
    }
    m_session = results.value(QStringLiteral("session_handle")).toString();
    if (!m_enabled) {
        closeSession();
        return;
    }
    bus.connect(kService, kPath, kInterface, QStringLiteral("Activated"), this,
                SLOT(onActivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));

    QList<PortalShortcut> shortcuts;
    for (const Action& action : m_actions) {
        shortcuts.append({action.id, {{QStringLiteral("description"), action.description}}});
    }
    const QString token = QStringLiteral("noctara%1").arg(++m_counter);
    m_pending = requestPath(token);
    bus.connect(kService, m_pending, kRequest, QStringLiteral("Response"), this, SLOT(onBound(uint, QVariantMap)));
    QDBusMessage call = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("BindShortcuts"));
    call << QVariant::fromValue(QDBusObjectPath(m_session)) << QVariant::fromValue(shortcuts) << QString()
         << QVariantMap{{QStringLiteral("handle_token"), token}};
    (void)bus.asyncCall(call);
}

void GlobalShortcuts::onBound(uint response, const QVariantMap& /*results*/)
{
    QDBusConnection::sessionBus().disconnect(kService, m_pending, kRequest, QStringLiteral("Response"), this,
                                             SLOT(onBound(uint, QVariantMap)));
    m_pending.clear();
    qCInfo(lcUi) << "global shortcuts:" << (response == 0 ? "bound" : "not bound") << m_actions.size() << "actions";
}

void GlobalShortcuts::onActivated(const QDBusObjectPath& session, const QString& id, qulonglong /*timestamp*/,
                                  const QVariantMap& /*options*/)
{
    if (session.path() == m_session) {
        Q_EMIT activated(id);
    }
}

void GlobalShortcuts::closeSession()
{
    if (m_session.isEmpty()) {
        return;
    }
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.disconnect(kService, kPath, kInterface, QStringLiteral("Activated"), this,
                   SLOT(onActivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));
    (void)bus.asyncCall(QDBusMessage::createMethodCall(
        kService, m_session, QStringLiteral("org.freedesktop.portal.Session"), QStringLiteral("Close")));
    m_session.clear();
}

}  // namespace noctara::ui
