#pragma once

#include <QString>
#include <QStringList>

namespace noctara::theme {

struct FontFamilies {
    QString heading;  // Manrope
    QString body;     // Inter
    QString mono;     // JetBrains Mono
};

// Registers the bundled OFL fonts from resources (once) and resolves user overrides with fallbacks.
class FontLoader {
public:
    [[nodiscard]] static FontFamilies loadBundled();

    // `requested` if it is installed, otherwise `bundled`. Logged when a requested family is missing.
    [[nodiscard]] static QString resolve(const QString& requested, const QString& bundled);

    // Explicit fallback chain: chosen family, bundled family, then broad-coverage system families
    // (Cyrillic, CJK, emoji) so mixed-script titles never render tofu.
    [[nodiscard]] static QStringList fallbackChain(const QString& primary, const QString& bundled);
};

}  // namespace noctara::theme
