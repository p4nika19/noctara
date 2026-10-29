#pragma once

#include <QObject>
#include <QRect>
#include <QStringList>
#include <QThreadPool>
#include <QVariantMap>

class QQuickWindow;

namespace noctara::settings {
class SettingsStore;
}

namespace noctara::ui {

// The window's last size, position and page (<state>/window.json).
struct WindowState {
    QRect geometry;  // invalid: never saved
    bool maximized = false;
    QString page;

    [[nodiscard]] QByteArray toJson() const;
    [[nodiscard]] static WindowState fromJson(const QByteArray& json);
};

// QML "Layout" singleton: the layout.* settings as typed properties (navigation, player bar, page views, track
// rows, full-screen player, window), plus the window state that is remembered between sessions.
class LayoutController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString navPosition READ navPosition NOTIFY changed)  // left | right | bottom
    Q_PROPERTY(QString navMode READ navMode NOTIFY changed)          // compact | expanded | auto_hide
    Q_PROPERTY(QStringList navItems READ navItems NOTIFY changed)
    Q_PROPERTY(QString playerPosition READ playerPosition NOTIFY changed)  // bottom | top
    Q_PROPERTY(QString playerSize READ playerSize NOTIFY changed)          // compact | regular | large
    Q_PROPERTY(bool playerWaveform READ playerWaveform NOTIFY changed)
    Q_PROPERTY(bool playerTime READ playerTime NOTIFY changed)
    Q_PROPERTY(bool playerVolume READ playerVolume NOTIFY changed)
    Q_PROPERTY(bool playerQueueButton READ playerQueueButton NOTIFY changed)
    Q_PROPERTY(QVariantMap views READ views NOTIFY changed)  // page → "list" | "grid"
    Q_PROPERTY(QString cardSize READ cardSize NOTIFY changed)
    Q_PROPERTY(bool rowPlays READ rowPlays NOTIFY changed)
    Q_PROPERTY(bool rowLikes READ rowLikes NOTIFY changed)
    Q_PROPERTY(bool rowReposts READ rowReposts NOTIFY changed)
    Q_PROPERTY(bool rowDuration READ rowDuration NOTIFY changed)
    Q_PROPERTY(QString fullPlayerArt READ fullPlayerArt NOTIFY changed)
    Q_PROPERTY(bool fullPlayerQueue READ fullPlayerQueue NOTIFY changed)
    Q_PROPERTY(bool fullPlayerWaveform READ fullPlayerWaveform NOTIFY changed)
    // Custom window decorations for this session (the setting takes effect after a restart).
    Q_PROPERTY(bool customDecorations READ customDecorations CONSTANT)
    Q_PROPERTY(bool closeToTray READ closeToTray NOTIFY changed)

public:
    // An empty state path keeps nothing between sessions.
    LayoutController(settings::SettingsStore& settings, QString statePath, QObject* parent = nullptr);
    ~LayoutController() override;
    LayoutController(const LayoutController&) = delete;
    LayoutController& operator=(const LayoutController&) = delete;
    LayoutController(LayoutController&&) = delete;
    LayoutController& operator=(LayoutController&&) = delete;

    [[nodiscard]] QString navPosition() const;
    [[nodiscard]] QString navMode() const;
    [[nodiscard]] QStringList navItems() const;
    [[nodiscard]] QString playerPosition() const;
    [[nodiscard]] QString playerSize() const;
    [[nodiscard]] bool playerWaveform() const;
    [[nodiscard]] bool playerTime() const;
    [[nodiscard]] bool playerVolume() const;
    [[nodiscard]] bool playerQueueButton() const;
    [[nodiscard]] QVariantMap views() const;
    [[nodiscard]] QString cardSize() const;
    [[nodiscard]] bool rowPlays() const;
    [[nodiscard]] bool rowLikes() const;
    [[nodiscard]] bool rowReposts() const;
    [[nodiscard]] bool rowDuration() const;
    [[nodiscard]] QString fullPlayerArt() const;
    [[nodiscard]] bool fullPlayerQueue() const;
    [[nodiscard]] bool fullPlayerWaveform() const;
    [[nodiscard]] bool customDecorations() const noexcept { return m_customDecorations; }
    [[nodiscard]] bool closeToTray() const;
    [[nodiscard]] bool startMinimized() const;

    // The page switch ("list" / "grid") writes the page's view setting.
    Q_INVOKABLE void setView(const QString& page, const QString& mode);

    // The page to open with: the start page setting, or the last page.
    [[nodiscard]] QString startPage() const;
    // Before the window is shown: the remembered size (and position, where the platform allows).
    void restore(QQuickWindow& window) const;
    // When the window closes: remember its size and page.
    void remember(const QQuickWindow& window, const QString& page);

Q_SIGNALS:
    void changed();

private:
    settings::SettingsStore& m_settings;
    QString m_statePath;
    WindowState m_state;
    bool m_customDecorations = false;
    QThreadPool m_writer;
};

}  // namespace noctara::ui
