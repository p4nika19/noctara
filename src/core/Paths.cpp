#include "core/Paths.hpp"

#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>

namespace noctara {

AppPaths AppPaths::standard()
{
    AppPaths paths;
    paths.configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    paths.cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    paths.stateDir = QStandardPaths::writableLocation(QStandardPaths::StateLocation);
    paths.dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);

    // The system layer is the first non-user entry of the config search path that has a config file.
    // Sessions prepend their own directories (Plasma ~/.config/kdedefaults, Ubuntu /etc/xdg/xdg-ubuntu),
    // so /etc/xdg is rarely first. Without any file, the first system directory is named.
    const QStringList configDirs = QStandardPaths::standardLocations(QStandardPaths::AppConfigLocation);
    for (const QString& dir : configDirs) {
        if (dir == paths.configDir) {
            continue;
        }
        const QString file = dir + QStringLiteral("/config.toml");
        if (paths.systemConfigFile.isEmpty()) {
            paths.systemConfigFile = file;
        }
        if (QFileInfo::exists(file)) {
            paths.systemConfigFile = file;
            break;
        }
    }
    return paths;
}

}  // namespace noctara
