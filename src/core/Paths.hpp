#pragma once

#include <QString>

namespace noctara {

inline constexpr auto kAppId = "noctara";

// Platform-standard locations. On Linux: ~/.config/noctara, ~/.cache/noctara (XDG_CACHE_HOME honoured),
// ~/.local/state/noctara. Requires QCoreApplication::applicationName to be set to kAppId.
struct AppPaths {
    QString configDir;
    QString systemConfigFile;  // lowest-precedence config layer (e.g. /etc/xdg/noctara/config.toml)
    QString cacheDir;
    QString stateDir;
    QString dataDir;  // installed components, such as the protected player (~/.local/share/noctara)

    [[nodiscard]] QString userConfigFile() const { return configDir + QStringLiteral("/config.toml"); }
    [[nodiscard]] QString themesDir() const { return configDir + QStringLiteral("/themes"); }

    [[nodiscard]] static AppPaths standard();
};

}  // namespace noctara
