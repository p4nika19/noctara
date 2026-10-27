#include "theme/FontLoader.hpp"

#include "core/Log.hpp"

#include <QFontDatabase>

namespace noctara::theme {

namespace {

QString registerFont(const QString& resource, const QString& expectedFamily)
{
    const int id = QFontDatabase::addApplicationFont(resource);
    if (id < 0) {
        qCWarning(lcTheme) << "failed to load bundled font" << resource;
        return expectedFamily;
    }
    const QStringList families = QFontDatabase::applicationFontFamilies(id);
    return families.isEmpty() ? expectedFamily : families.front();
}

}  // namespace

FontFamilies FontLoader::loadBundled()
{
    static const FontFamilies families{
        registerFont(QStringLiteral(":/fonts/Manrope-Variable.ttf"), QStringLiteral("Manrope")),
        registerFont(QStringLiteral(":/fonts/Inter-Variable.ttf"), QStringLiteral("Inter")),
        registerFont(QStringLiteral(":/fonts/JetBrainsMono-Variable.ttf"), QStringLiteral("JetBrains Mono")),
    };
    return families;
}

QString FontLoader::resolve(const QString& requested, const QString& bundled)
{
    QString trimmed = requested.trimmed();
    if (trimmed.isEmpty()) {
        return bundled;
    }
    if (QFontDatabase::hasFamily(trimmed)) {
        return trimmed;
    }
    qCInfo(lcTheme) << "font family" << trimmed << "is not installed; using" << bundled;
    return bundled;
}

QStringList FontLoader::fallbackChain(const QString& primary, const QString& bundled)
{
    QStringList chain{primary};
    if (bundled != primary) {
        chain.append(bundled);
    }
    chain.append({QStringLiteral("Noto Sans"), QStringLiteral("Noto Sans CJK SC"), QStringLiteral("Noto Color Emoji"),
                  QStringLiteral("DejaVu Sans")});
    return chain;
}

}  // namespace noctara::theme
