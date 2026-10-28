#include "ui/TrayIcon.hpp"

#include "i18n/LanguageManager.hpp"
#include "media/PlaybackController.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"

#include <QApplication>
#include <QIcon>
#include <QMenu>
#include <QSystemTrayIcon>

namespace noctara::ui {

TrayIcon::TrayIcon(media::PlaybackController& player, settings::SettingsStore& settings,
                   i18n::LanguageManager& languages, QObject* parent)
    : QObject(parent)
    , m_player(player)
    , m_settings(settings)
{
    connect(&m_settings, &settings::SettingsStore::valueChanged, this, [this](const QString& key) {
        if (key == QLatin1StringView(settings::keys::TrayIcon)) {
            apply();
        }
    });
    connect(&languages, &i18n::LanguageManager::languageChanged, this, &TrayIcon::retranslate);
    connect(&m_player, &media::PlaybackController::trackChanged, this, &TrayIcon::refresh);
    connect(&m_player, &media::PlaybackController::stateChanged, this, &TrayIcon::refresh);
    connect(&m_player, &media::PlaybackController::queueChanged, this, &TrayIcon::refresh);
    apply();
}

TrayIcon::~TrayIcon() = default;

void TrayIcon::apply()
{
    // The offscreen platform (tests, screenshots) has no tray; neither has a plain QGuiApplication.
    const bool possible = qobject_cast<QApplication*>(QCoreApplication::instance()) != nullptr &&
                          QGuiApplication::platformName() != u"offscreen" && QSystemTrayIcon::isSystemTrayAvailable();
    const bool wanted = possible && m_settings.boolValue(settings::keys::TrayIcon);
    if (wanted && !m_icon) {
        create();
    } else if (!wanted && m_icon) {
        m_icon.reset();
        m_menu.reset();
    }
}

void TrayIcon::create()
{
    m_menu = std::make_unique<QMenu>();
    m_playPause = m_menu->addAction(QString(), &m_player, &media::PlaybackController::playPause);
    m_previous = m_menu->addAction(QString(), &m_player, &media::PlaybackController::previous);
    m_next = m_menu->addAction(QString(), &m_player, &media::PlaybackController::next);
    m_menu->addSeparator();
    m_show = m_menu->addAction(QString(), this, &TrayIcon::showRequested);
    m_quit = m_menu->addAction(QString(), qApp, &QCoreApplication::quit);

    // The D-Bus tray sends pixmaps at the icon's available sizes, and an SVG alone has none: render them.
    const QIcon scalable(QStringLiteral(":/app/noctara.svg"));
    QIcon icon;
    for (const int side : {16, 22, 24, 32, 48, 64, 128}) {
        icon.addPixmap(scalable.pixmap(QSize(side, side)));
    }
    m_icon = std::make_unique<QSystemTrayIcon>(icon);
    m_icon->setContextMenu(m_menu.get());
    connect(m_icon.get(), &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
            Q_EMIT showRequested();
        } else if (reason == QSystemTrayIcon::MiddleClick) {
            m_player.playPause();
        }
    });
    retranslate();
    m_icon->show();
}

void TrayIcon::retranslate()
{
    if (!m_icon) {
        return;
    }
    m_previous->setText(tr("Previous"));
    m_next->setText(tr("Next"));
    m_show->setText(tr("Show Noctara"));
    m_quit->setText(tr("Quit"));
    refresh();
}

void TrayIcon::refresh()
{
    if (!m_icon) {
        return;
    }
    m_playPause->setText(m_player.playing() ? tr("Pause") : tr("Play"));
    m_playPause->setEnabled(m_player.hasTrack());
    m_previous->setEnabled(m_player.hasTrack());
    m_next->setEnabled(m_player.canGoNext());
    // SoundCloud content is shown as it is; only the frame is translated.
    m_icon->setToolTip(m_player.hasTrack() ? tr("%1 — %2").arg(m_player.title(), m_player.artist())
                                           : QStringLiteral("Noctara"));
}

}  // namespace noctara::ui
