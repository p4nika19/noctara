#pragma once

#include "gfx/PowerMonitor.hpp"

#include <QObject>
#include <QString>

namespace noctara::settings {
class SettingsStore;
}
namespace noctara::i18n {
class LanguageManager;
}

namespace noctara::ui {

class SettingsController;

// Translated explanation of what the energy-saving setting is doing right now, given the power source.
[[nodiscard]] QString powerStatusText(const QString& mode, gfx::PowerState state);

// Keeps the status line of the energy-saving row in sync with the power source, the setting and the language.
class PowerStatusPresenter : public QObject {
    Q_OBJECT

public:
    PowerStatusPresenter(gfx::PowerMonitor& monitor, settings::SettingsStore& settings,
                         i18n::LanguageManager& languages, SettingsController& controller, QObject* parent = nullptr);

private:
    void refresh();

    gfx::PowerMonitor& m_monitor;
    settings::SettingsStore& m_settings;
    SettingsController& m_controller;
};

}  // namespace noctara::ui
