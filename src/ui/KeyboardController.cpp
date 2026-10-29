#include "ui/KeyboardController.hpp"

#include "i18n/LanguageManager.hpp"
#include "settings/Keys.hpp"
#include "settings/Schema.hpp"
#include "settings/SettingsStore.hpp"
#include "ui/SettingsController.hpp"

#include <QCoreApplication>
#include <QKeySequence>

namespace noctara::ui {

namespace {

QString latin(std::string_view text)
{
    return QString::fromLatin1(text.data(), static_cast<qsizetype>(text.size()));
}

template <class Visit> void forEachShortcut(const settings::Schema& schema, Visit visit)
{
    for (const settings::Definition& def : schema.definitions()) {
        if (def.type == settings::Type::Shortcut && def.key.starts_with(KeyboardController::kPrefix)) {
            visit(def, latin(def.key.substr(KeyboardController::kPrefix.size())));
        }
    }
}

}  // namespace

KeyboardController::KeyboardController(settings::SettingsStore& settings, i18n::LanguageManager& languages,
                                       SettingsController& settingsUi, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_settingsUi(settingsUi)
{
    connect(&m_settings, &settings::SettingsStore::valueChanged, this, [this](const QString& key) {
        if (key.startsWith(latin(kPrefix))) {
            refresh();
        } else if (key == QLatin1StringView(settings::keys::GlobalShortcuts)) {
            applyGlobal();
        }
    });
    connect(&languages, &i18n::LanguageManager::languageChanged, this, &KeyboardController::refresh);
    connect(&m_global, &GlobalShortcuts::activated, this, &KeyboardController::globalActivated);
    refresh();
    applyGlobal();
}

QVariantMap KeyboardController::sequences() const
{
    QVariantMap out;
    forEachShortcut(m_settings.schema(), [&](const settings::Definition& def, const QString& action) {
        out.insert(action, m_settings.stringValue(def.key));
    });
    return out;
}

QVariantList KeyboardController::cheatSheet() const
{
    QVariantList out;
    forEachShortcut(m_settings.schema(), [&](const settings::Definition& def, const QString& action) {
        const QString keys = m_settings.stringValue(def.key);
        if (!keys.isEmpty()) {
            out.append(QVariantMap{{QStringLiteral("action"), action},
                                   {QStringLiteral("label"), QCoreApplication::translate("Settings", def.label)},
                                   {QStringLiteral("keys"), display(keys)}});
        }
    });
    return out;
}

QVariantMap KeyboardController::conflicts() const
{
    QHash<QString, QStringList> byKeys;
    forEachShortcut(m_settings.schema(), [&](const settings::Definition& def, const QString&) {
        const QString keys = m_settings.stringValue(def.key);
        if (!keys.isEmpty()) {
            // Compare the parsed sequences, so "ctrl+k" and "Ctrl+K" are the same.
            byKeys[QKeySequence::fromString(keys, QKeySequence::PortableText).toString(QKeySequence::PortableText)]
                .append(latin(def.key));
        }
    });
    QVariantMap out;
    for (auto it = byKeys.cbegin(); it != byKeys.cend(); ++it) {
        if (it.value().size() < 2) {
            continue;
        }
        for (const QString& key : it.value()) {
            QStringList others;
            for (const QString& other : it.value()) {
                if (other != key) {
                    const settings::Definition* def = m_settings.schema().find(other.toStdString());
                    others.append(QCoreApplication::translate("Settings", def->label));
                }
            }
            out.insert(key, others);
        }
    }
    return out;
}

void KeyboardController::refresh()
{
    const QVariantMap clashes = conflicts();
    forEachShortcut(m_settings.schema(), [&](const settings::Definition& def, const QString&) {
        const QString key = latin(def.key);
        const QStringList others = clashes.value(key).toStringList();
        m_settingsUi.setRowStatus(
            key, others.isEmpty()
                     ? QString()
                     : tr("Also used by: %1. Only one of them works.").arg(others.join(QStringLiteral(", "))));
    });
    Q_EMIT changed();
}

void KeyboardController::applyGlobal()
{
    m_global.setEnabled(m_settings.boolValue(settings::keys::GlobalShortcuts),
                        {{QStringLiteral("play_pause"), tr("Play or pause")},
                         {QStringLiteral("next"), tr("Next track")},
                         {QStringLiteral("previous"), tr("Previous track")},
                         {QStringLiteral("show"), tr("Show Noctara")}});
}

QString KeyboardController::fromKeyEvent(int key, int modifiers)
{
    switch (key) {
    case Qt::Key_Control:
    case Qt::Key_Shift:
    case Qt::Key_Alt:
    case Qt::Key_Meta:
    case Qt::Key_AltGr:
    case Qt::Key_unknown: return {};
    default: break;
    }
    const auto mods = Qt::KeyboardModifiers(modifiers) &
                      (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
    return QKeySequence(QKeyCombination(mods, static_cast<Qt::Key>(key))).toString(QKeySequence::PortableText);
}

QString KeyboardController::display(const QString& portable)
{
    return QKeySequence::fromString(portable, QKeySequence::PortableText).toString(QKeySequence::NativeText);
}

}  // namespace noctara::ui
