#include "ui/PowerStatus.hpp"

#include "i18n/LanguageManager.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"
#include "ui/SettingsController.hpp"

#include <QCoreApplication>

namespace noctara::ui {

namespace {

QString key()
{
    return QString::fromLatin1(settings::keys::EnergySaving.data(),
                               static_cast<qsizetype>(settings::keys::EnergySaving.size()));
}

}  // namespace

QString powerStatusText(const QString& mode, gfx::PowerState state)
{
    if (mode == u"always") {
        return QCoreApplication::translate("Power", "Active now: energy saving is always on.");
    }
    if (mode == u"off") {
        return QCoreApplication::translate("Power", "Inactive: energy saving is turned off.");
    }
    switch (state) {
    case gfx::PowerState::OnBattery:
        return QCoreApplication::translate("Power", "Active now: this computer is running on battery.");
    case gfx::PowerState::OnMains:
        return QCoreApplication::translate(
            "Power", "Inactive: this computer is plugged in. It turns on by itself when you unplug.");
    case gfx::PowerState::NoBattery:
        return QCoreApplication::translate(
            "Power", "Inactive: no battery was found on this computer. Choose Always to save energy anyway.");
    case gfx::PowerState::Unknown: break;
    }
    return QCoreApplication::translate(
        "Power", "Inactive: the power source cannot be detected (UPower is not running). Choose Always to save "
                 "energy anyway.");
}

PowerStatusPresenter::PowerStatusPresenter(gfx::PowerMonitor& monitor, settings::SettingsStore& settings,
                                           i18n::LanguageManager& languages, SettingsController& controller,
                                           QObject* parent)
    : QObject(parent)
    , m_monitor(monitor)
    , m_settings(settings)
    , m_controller(controller)
{
    connect(&m_monitor, &gfx::PowerMonitor::stateChanged, this, &PowerStatusPresenter::refresh);
    connect(&languages, &i18n::LanguageManager::languageChanged, this, &PowerStatusPresenter::refresh);
    connect(&m_settings, &settings::SettingsStore::valueChanged, this, [this](const QString& changed) {
        if (changed == key()) {
            refresh();
        }
    });
    refresh();
}

void PowerStatusPresenter::refresh()
{
    m_controller.setRowStatus(key(),
                              powerStatusText(m_settings.stringValue(settings::keys::EnergySaving), m_monitor.state()));
}

}  // namespace noctara::ui
