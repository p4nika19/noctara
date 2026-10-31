#include "core/Paths.hpp"

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

    // The system layer is the first non-user entry of the config search path.
    const QStringList configDirs = QStandardPaths::standardLocations(QStandardPaths::AppConfigLocation);
    for (const QString& dir : configDirs) {
        if (dir != paths.configDir) {
            paths.systemConfigFile = dir + QStringLiteral("/config.toml");
            break;
        }
    }
    return paths;
}

}  // namespace noctara
