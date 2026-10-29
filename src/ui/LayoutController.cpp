#include "ui/LayoutController.hpp"

#include "core/FileIo.hpp"
#include "core/Log.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"

#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickWindow>
#include <QScreen>

namespace noctara::ui {

namespace keys = settings::keys;

QByteArray WindowState::toJson() const
{
    const QJsonObject object{{QStringLiteral("version"), 1},
                             {QStringLiteral("x"), geometry.x()},
                             {QStringLiteral("y"), geometry.y()},
                             {QStringLiteral("width"), geometry.width()},
                             {QStringLiteral("height"), geometry.height()},
                             {QStringLiteral("maximized"), maximized},
                             {QStringLiteral("page"), page}};
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

WindowState WindowState::fromJson(const QByteArray& json)
{
    WindowState state;
    const QJsonObject object = QJsonDocument::fromJson(json).object();
    if (object.value(QStringLiteral("version")).toInt() != 1) {
        return state;
    }
    const QRect rect(object.value(QStringLiteral("x")).toInt(), object.value(QStringLiteral("y")).toInt(),
                     object.value(QStringLiteral("width")).toInt(), object.value(QStringLiteral("height")).toInt());
    // Reject nonsense (a damaged file must not open a 1×1 or 100 000 px window).
    if (rect.width() >= 400 && rect.height() >= 300 && rect.width() <= 16384 && rect.height() <= 16384) {
        state.geometry = rect;
    }
    state.maximized = object.value(QStringLiteral("maximized")).toBool();
    state.page = object.value(QStringLiteral("page")).toString();
    return state;
}

LayoutController::LayoutController(settings::SettingsStore& settings, QString statePath, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_statePath(std::move(statePath))
    , m_customDecorations(settings.stringValue(keys::WindowDecorations) == u"custom")
{
    m_writer.setMaxThreadCount(1);
    if (!m_statePath.isEmpty()) {
        if (const Result<QByteArray> data = readWholeFile(m_statePath)) {
            m_state = WindowState::fromJson(*data);
        }
    }
    connect(&m_settings, &settings::SettingsStore::valueChanged, this, [this](const QString& key) {
        if (key.startsWith(u"layout.")) {
            Q_EMIT changed();
        }
    });
}

LayoutController::~LayoutController()
{
    m_writer.waitForDone();
}

QString LayoutController::navPosition() const
{
    return m_settings.stringValue(keys::NavPosition);
}

QString LayoutController::navMode() const
{
    return m_settings.stringValue(keys::NavMode);
}

QStringList LayoutController::navItems() const
{
    return m_settings.value(keys::NavItems).toStringList();
}

QString LayoutController::playerPosition() const
{
    return m_settings.stringValue(keys::PlayerPosition);
}

QString LayoutController::playerSize() const
{
    return m_settings.stringValue(keys::PlayerSize);
}

bool LayoutController::playerWaveform() const
{
    return m_settings.boolValue(keys::PlayerWaveform);
}

bool LayoutController::playerTime() const
{
    return m_settings.boolValue(keys::PlayerTime);
}

bool LayoutController::playerVolume() const
{
    return m_settings.boolValue(keys::PlayerVolume);
}

bool LayoutController::playerQueueButton() const
{
    return m_settings.boolValue(keys::PlayerQueueButton);
}

QVariantMap LayoutController::views() const
{
    return {{QStringLiteral("library"), m_settings.stringValue(keys::ViewLibrary)},
            {QStringLiteral("likes"), m_settings.stringValue(keys::ViewLikes)},
            {QStringLiteral("history"), m_settings.stringValue(keys::ViewHistory)},
            {QStringLiteral("charts"), m_settings.stringValue(keys::ViewCharts)}};
}

void LayoutController::setView(const QString& page, const QString& mode)
{
    std::string_view key;
    if (page == u"library") {
        key = keys::ViewLibrary;
    } else if (page == u"likes") {
        key = keys::ViewLikes;
    } else if (page == u"history") {
        key = keys::ViewHistory;
    } else if (page == u"charts") {
        key = keys::ViewCharts;
    } else {
        return;
    }
    if (const Result<void> set = m_settings.setUserValue(key, mode); !set) {
        qCInfo(lcUi).noquote() << "view not changed:" << QString::fromStdString(set.error().describe());
    }
}

QString LayoutController::cardSize() const
{
    return m_settings.stringValue(keys::CardSize);
}

bool LayoutController::rowPlays() const
{
    return m_settings.boolValue(keys::RowPlays);
}

bool LayoutController::rowLikes() const
{
    return m_settings.boolValue(keys::RowLikes);
}

bool LayoutController::rowReposts() const
{
    return m_settings.boolValue(keys::RowReposts);
}

bool LayoutController::rowDuration() const
{
    return m_settings.boolValue(keys::RowDuration);
}

QString LayoutController::fullPlayerArt() const
{
    return m_settings.stringValue(keys::FullPlayerArt);
}

bool LayoutController::fullPlayerQueue() const
{
    return m_settings.boolValue(keys::FullPlayerQueue);
}

bool LayoutController::fullPlayerWaveform() const
{
    return m_settings.boolValue(keys::FullPlayerWaveform);
}

bool LayoutController::closeToTray() const
{
    return m_settings.boolValue(keys::CloseToTray);
}

bool LayoutController::startMinimized() const
{
    return m_settings.boolValue(keys::StartMinimized);
}

QString LayoutController::startPage() const
{
    QString page = m_settings.stringValue(keys::StartPage);
    if (page != u"last") {
        return page;
    }
    return m_state.page.isEmpty() || m_state.page == u"settings" ? QStringLiteral("home") : m_state.page;
}

void LayoutController::restore(QQuickWindow& window) const
{
    if (!m_settings.boolValue(keys::RememberGeometry) || !m_state.geometry.isValid()) {
        return;
    }
    window.resize(m_state.geometry.size());
    // Positions only mean something where the platform lets applications place windows (not on Wayland), and
    // only when the spot is still on a screen.
    if (QGuiApplication::platformName() != u"wayland") {
        const QScreen* screen = QGuiApplication::screenAt(m_state.geometry.center());
        if (screen != nullptr) {
            window.setPosition(m_state.geometry.topLeft());
        }
    }
    if (m_state.maximized) {
        window.setWindowStates(Qt::WindowMaximized);
    }
}

void LayoutController::remember(const QQuickWindow& window, const QString& page)
{
    WindowState state;
    state.maximized = (window.windowStates() & Qt::WindowMaximized) != 0;
    // While maximized the normal size is not known; keep the one from before.
    state.geometry = state.maximized ? m_state.geometry : window.geometry();
    state.page = page;
    if (m_statePath.isEmpty() ||
        (state.geometry == m_state.geometry && state.maximized == m_state.maximized && state.page == m_state.page)) {
        return;
    }
    m_state = state;
    m_writer.start([path = m_statePath, data = state.toJson()] {
        Result<void> written = ensureDirectory(path.section(u'/', 0, -2));
        if (written) {
            written = writeFileAtomic(path, data, 0600, Durability::Atomic);
        }
        if (!written) {
            qCWarning(lcUi).noquote() << "could not save the window state:"
                                      << QString::fromStdString(written.error().describe());
        }
    });
}

}  // namespace noctara::ui
