// The application's settings schema: the single source of truth for the settings UI, config file,
// CLI/env overrides and docs/configuration.md. Adding a setting = adding one entry here.
#include "core/Log.hpp"
#include "core/NetAddress.hpp"
#include "settings/Keys.hpp"
#include "settings/Schema.hpp"

#include <QDir>
#include <QUrl>

#include <algorithm>
#include <format>
#include <tuple>

// Entries rely on designated initializers leaving defaulted fields out; that is the intended idiom here.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"

namespace noctara::settings {

namespace {

using enum Category;

QVariant str(const char* text)
{
    return QVariant::fromValue(QString::fromUtf8(text));
}

QVariant integer(qint64 value)
{
    return QVariant::fromValue(value);
}

QVariant real(double value)
{
    return QVariant::fromValue(value);
}

Result<void> invalid(const QString& item, const char* expected)
{
    return fail(ErrorCode::ConfigInvalidValue, std::format("'{}' is not {}", item.toStdString(), expected));
}

// Navigation pages: known ids, each at most once.
Result<void> validateNavItems(const QVariant& value)
{
    static const QStringList known{QStringLiteral("home"),  QStringLiteral("search"),  QStringLiteral("library"),
                                   QStringLiteral("likes"), QStringLiteral("history"), QStringLiteral("charts")};
    QStringList seen;
    for (const QString& item : value.toStringList()) {
        if (!known.contains(item)) {
            return invalid(item, "home, search, library, likes, history or charts");
        }
        if (seen.contains(item)) {
            return invalid(item, "listed once");
        }
        seen.append(item);
    }
    return {};
}

// QKeySequence portable text of a single chord: modifiers then one key ("Ctrl+Shift+Q", "Space", "Ctrl+,"),
// or empty for no shortcut. (QKeySequence itself lives in Qt GUI, which settings does not depend on.)
Result<void> validateShortcut(const QVariant& value)
{
    const QString text = value.toString();
    if (text.isEmpty()) {
        return {};
    }
    static const QStringList modifiers{QStringLiteral("Ctrl"), QStringLiteral("Shift"), QStringLiteral("Alt"),
                                       QStringLiteral("Meta")};
    // The key may itself be "+" or ",": split the modifiers off the front.
    QString rest = text;
    while (true) {
        const qsizetype plus = rest.indexOf(u'+');
        if (plus <= 0 || plus == rest.size() - 1 || !modifiers.contains(rest.left(plus))) {
            break;
        }
        rest = rest.mid(plus + 1);
    }
    if (rest.isEmpty() || (rest.size() > 1 && (rest.contains(u',') || rest.endsWith(u'+'))) ||
        modifiers.contains(rest)) {
        return invalid(text, "one key with optional Ctrl+, Shift+, Alt+ or Meta+ (e.g. Ctrl+Shift+Q)");
    }
    return {};
}

Result<void> validateHost(const QVariant& value)
{
    const QString host = value.toString();
    if (host.isEmpty() || IpAddress::parse(host) || isValidHostname(host)) {
        return {};
    }
    return invalid(host, "a host name or IP address");
}

Result<void> validateDnsServers(const QVariant& value)
{
    for (const QString& item : value.toStringList()) {
        if (!Endpoint::parse(item, 53)) {
            return invalid(item, "an IP address with an optional port");
        }
    }
    return {};
}

Result<void> validateAddresses(const QVariant& value)
{
    for (const QString& item : value.toStringList()) {
        if (!IpAddress::parse(item)) {
            return invalid(item, "an IP address");
        }
    }
    return {};
}

Result<void> validateClientId(const QVariant& value)
{
    const QString id = value.toString().trimmed();
    const bool alphanumeric =
        std::ranges::all_of(id, [](QChar c) { return c.isDigit() || (c.isLetter() && c.unicode() < 128); });
    if (id.isEmpty() || (id.size() >= 24 && id.size() <= 64 && alphanumeric)) {
        return {};
    }
    return invalid(id, "a client ID (24–64 letters and digits)");
}

Result<void> validateDohUrl(const QVariant& value)
{
    const QString text = value.toString();
    if (text.isEmpty()) {
        return {};
    }
    const QUrl url(text, QUrl::StrictMode);
    const bool schemeOk = url.scheme() == u"https" || url.scheme() == u"http";
    if (!url.isValid() || !schemeOk || url.host().isEmpty() || !url.userInfo().isEmpty()) {
        return invalid(log::redactUrlCredentials(text), "an https:// URL");  // the detail reaches the log
    }
    return {};
}

// "host=address" pairs; a host may appear several times to give it more than one address.
Result<void> validateHostOverrides(const QVariant& value)
{
    for (const QString& item : value.toStringList()) {
        const qsizetype eq = item.indexOf(u'=');
        if (eq < 0 || !isValidHostname(QStringView(item).first(eq).trimmed()) ||
            !IpAddress::parse(QStringView(item).sliced(eq + 1))) {
            return invalid(item, "of the form host=address");
        }
    }
    return {};
}

// Host names (matching subdomains too), "*.domain", ".domain", IP addresses, CIDR ranges, "<local>" or "*".
Result<void> validateBypass(const QVariant& value)
{
    for (const QString& item : value.toStringList()) {
        QStringView rule = QStringView(item).trimmed();
        if (rule == u"*" || rule == u"<local>" || Cidr::parse(rule)) {
            continue;
        }
        if (rule.startsWith(u"*.")) {
            rule = rule.sliced(2);
        } else if (rule.startsWith(u'.')) {
            rule = rule.sliced(1);
        }
        if (!isValidHostname(rule)) {
            return invalid(item, "a host name, *.domain, IP address or CIDR range");
        }
    }
    return {};
}

Result<void> validateCacheDir(const QVariant& value)
{
    const QString dir = value.toString();
    if (dir.isEmpty() || QDir::isAbsolutePath(dir)) {
        return {};
    }
    return invalid(dir, "an absolute path");
}

std::vector<Section> builtinSections()
{
    return {
        {"general", General, QT_TRANSLATE_NOOP("Settings", "Language"),
         QT_TRANSLATE_NOOP("Settings", "Interface language.")},
        {"general.formatting", General, QT_TRANSLATE_NOOP("Settings", "Formatting"),
         QT_TRANSLATE_NOOP("Settings", "How times and numbers are displayed.")},

        {"appearance.theme", Appearance, QT_TRANSLATE_NOOP("Settings", "Theme"),
         QT_TRANSLATE_NOOP("Settings", "Midnight (dark), Frost (light) or follow the system.")},
        {"appearance.accent", Appearance, QT_TRANSLATE_NOOP("Settings", "Accent color"),
         QT_TRANSLATE_NOOP("Settings", "The whole palette is generated from a single accent color.")},
        {"appearance.backdrop", Appearance, QT_TRANSLATE_NOOP("Settings", "Backdrop"),
         QT_TRANSLATE_NOOP("Settings", "The light behind the interface.")},
        {"appearance.glow", Appearance, QT_TRANSLATE_NOOP("Settings", "Glow"),
         QT_TRANSLATE_NOOP("Settings", "Soft accent-coloured light around artwork, controls and progress.")},
        {"appearance.glass", Appearance, QT_TRANSLATE_NOOP("Settings", "Glass"),
         QT_TRANSLATE_NOOP("Settings", "How translucent surfaces are rendered.")},
        {"appearance.motion", Appearance, QT_TRANSLATE_NOOP("Settings", "Motion"),
         QT_TRANSLATE_NOOP("Settings", "Animations and transitions.")},
        {"appearance.shape", Appearance, QT_TRANSLATE_NOOP("Settings", "Shape"),
         QT_TRANSLATE_NOOP("Settings", "Corner rounding of surfaces and artwork.")},
        {"appearance.text", Appearance, QT_TRANSLATE_NOOP("Settings", "Text and scale"),
         QT_TRANSLATE_NOOP("Settings", "Interface zoom and fonts. Missing fonts fall back to the bundled ones.")},
        {"appearance.icons", Appearance, QT_TRANSLATE_NOOP("Settings", "Icons"),
         QT_TRANSLATE_NOOP("Settings", "Icon appearance.")},

        {"layout.content", Layout, QT_TRANSLATE_NOOP("Settings", "Content"),
         QT_TRANSLATE_NOOP("Settings", "How densely pages are laid out.")},
        {"layout.navigation", Layout, QT_TRANSLATE_NOOP("Settings", "Navigation"),
         QT_TRANSLATE_NOOP("Settings", "Where the navigation sits and what it shows.")},
        {"layout.player", Layout, QT_TRANSLATE_NOOP("Settings", "Player bar"),
         QT_TRANSLATE_NOOP("Settings", "The bar with the current track.")},
        {"layout.views", Layout, QT_TRANSLATE_NOOP("Settings", "Pages"),
         QT_TRANSLATE_NOOP("Settings", "How each page shows its items.")},
        {"layout.rows", Layout, QT_TRANSLATE_NOOP("Settings", "Track rows"),
         QT_TRANSLATE_NOOP("Settings", "What a track row shows besides the title and artist.")},
        {"layout.full_player", Layout, QT_TRANSLATE_NOOP("Settings", "Full-screen player"),
         QT_TRANSLATE_NOOP("Settings", "The large player with the cover art.")},
        {"layout.window", Layout, QT_TRANSLATE_NOOP("Settings", "Window"),
         QT_TRANSLATE_NOOP("Settings", "The window frame, size and what happens at start and close.")},

        {"keyboard.shortcuts", Keyboard, QT_TRANSLATE_NOOP("Settings", "Shortcuts"),
         QT_TRANSLATE_NOOP("Settings", "Click a shortcut and press the new keys. Backspace removes it, Escape "
                                       "cancels. Media keys always work.")},
        {"keyboard.global", Keyboard, QT_TRANSLATE_NOOP("Settings", "Global shortcuts"),
         QT_TRANSLATE_NOOP("Settings", "Shortcuts that work while Noctara is in the background.")},

        {"playback.stream", Playback, QT_TRANSLATE_NOOP("Settings", "Sound"),
         QT_TRANSLATE_NOOP("Settings", "Which of SoundCloud's streams is played.")},
        {"playback.transitions", Playback, QT_TRANSLATE_NOOP("Settings", "Transitions"),
         QT_TRANSLATE_NOOP("Settings", "How one track leads into the next.")},
        {"playback.volume", Playback, QT_TRANSLATE_NOOP("Settings", "Volume"),
         QT_TRANSLATE_NOOP("Settings", "Volume when Noctara starts.")},
        {"playback.queue", Playback, QT_TRANSLATE_NOOP("Settings", "Queue"),
         QT_TRANSLATE_NOOP("Settings", "What happens when the queue runs out.")},
        {"playback.desktop", Playback, QT_TRANSLATE_NOOP("Settings", "Desktop integration"),
         QT_TRANSLATE_NOOP("Settings", "Controlling playback from outside the window.")},
        {"playback.protected", Playback, QT_TRANSLATE_NOOP("Settings", "Protected tracks"),
         QT_TRANSLATE_NOOP("Settings", "Tracks SoundCloud streams only encrypted (DRM). They play in a small "
                                       "background player with Widevine and are never stored on this computer.")},

        {"performance.gpu", Performance, QT_TRANSLATE_NOOP("Settings", "Graphics"),
         QT_TRANSLATE_NOOP("Settings", "GPU acceleration. Changes take effect after a restart.")},
        {"performance.rendering", Performance, QT_TRANSLATE_NOOP("Settings", "Rendering"),
         QT_TRANSLATE_NOOP("Settings", "Frame pacing.")},
        {"performance.power", Performance, QT_TRANSLATE_NOOP("Settings", "Power"),
         QT_TRANSLATE_NOOP("Settings",
                           "How Noctara reduces its own energy use. Your system power profile is not changed.")},

        {"network.usage", Network, QT_TRANSLATE_NOOP("Settings", "Data usage"),
         QT_TRANSLATE_NOOP("Settings", "When and how much Noctara downloads.")},
        {"network.dns", Network, QT_TRANSLATE_NOOP("Settings", "DNS"),
         QT_TRANSLATE_NOOP("Settings", "How server names are turned into addresses. Applies to all of Noctara's "
                                       "connections without a restart.")},
        {"network.dns.cache", Network, QT_TRANSLATE_NOOP("Settings", "DNS cache"),
         QT_TRANSLATE_NOOP("Settings", "How long answers are remembered. Lifetimes from the DNS server are kept "
                                       "within these bounds.")},
        {"network.proxy", Network, QT_TRANSLATE_NOOP("Settings", "Proxy"),
         QT_TRANSLATE_NOOP("Settings", "Route Noctara's traffic through a proxy server.")},
        {"network.connection", Network, QT_TRANSLATE_NOOP("Settings", "Connections"),
         QT_TRANSLATE_NOOP("Settings", "Protocol, time limits and retries.")},
        {"network.tls", Network, QT_TRANSLATE_NOOP("Settings", "Security"),
         QT_TRANSLATE_NOOP("Settings", "Encrypted connections. Certificates are always verified.")},

        {"cache", Storage, QT_TRANSLATE_NOOP("Settings", "Audio cache"),
         QT_TRANSLATE_NOOP("Settings", "Tracks you play are kept on this computer, so replays, seeking and the next "
                                       "track start instantly and use no data. Protected tracks are never stored.")},
        {"cache.images", Storage, QT_TRANSLATE_NOOP("Settings", "Images"),
         QT_TRANSLATE_NOOP("Settings", "Cover art and profile pictures.")},
        {"cache.metadata", Storage, QT_TRANSLATE_NOOP("Settings", "Metadata"),
         QT_TRANSLATE_NOOP("Settings", "Track, playlist and profile information from SoundCloud, so pages open "
                                       "quickly and work offline.")},

        {"library.loading", Library, QT_TRANSLATE_NOOP("Settings", "Loading"),
         QT_TRANSLATE_NOOP("Settings", "How lists of tracks, playlists and people are fetched from SoundCloud.")},

        {"soundcloud", Advanced, QT_TRANSLATE_NOOP("Settings", "SoundCloud"),
         QT_TRANSLATE_NOOP("Settings", "How Noctara talks to SoundCloud's web API.")},
    };
}

std::vector<Definition> builtinDefinitions()
{
    std::vector<Definition> defs;

    // --- General -------------------------------------------------------------------------------
    defs.push_back({.key = keys::Language,
                    .type = Type::Enum,
                    .defaultValue = str("system"),
                    .options = {{"system", QT_TRANSLATE_NOOP("Settings", "System default")},
                                {"en", "English", false},
                                {"ru", "Русский", false}},
                    .category = General,
                    .label = QT_TRANSLATE_NOOP("Settings", "Language"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Applied immediately, without a restart."),
                    .cliAlias = "lang"});
    defs.push_back({.key = keys::TimeFormat,
                    .type = Type::Enum,
                    .defaultValue = str("auto"),
                    .options = {{"auto", QT_TRANSLATE_NOOP("Settings", "Follow language")},
                                {"12h", QT_TRANSLATE_NOOP("Settings", "12-hour")},
                                {"24h", QT_TRANSLATE_NOOP("Settings", "24-hour")}},
                    .category = General,
                    .label = QT_TRANSLATE_NOOP("Settings", "Time format"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Clock style for dates and times.")});
    defs.push_back({.key = keys::CompactNumbers,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = General,
                    .label = QT_TRANSLATE_NOOP("Settings", "Compact numbers"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Show large counts as 1.2K instead of 1,234.")});

    // --- Appearance: theme -----------------------------------------------------------------------
    defs.push_back({.key = keys::ThemeMode,
                    .type = Type::Enum,
                    .defaultValue = str("system"),
                    .options = {{"system", QT_TRANSLATE_NOOP("Settings", "System")},
                                {"dark", QT_TRANSLATE_NOOP("Settings", "Midnight (dark)")},
                                {"light", QT_TRANSLATE_NOOP("Settings", "Frost (light)")}},
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Theme"),
                    .description = QT_TRANSLATE_NOOP("Settings", "System follows the desktop color scheme live."),
                    // Edited by the Auto / Midnight / Frost tiles at the top of the Appearance page.
                    .flags = flag::ConfigOnly,
                    .cliAlias = "theme"});
    defs.push_back({.key = keys::CustomTheme,
                    .type = Type::String,
                    .defaultValue = str(""),
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Custom theme"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "A theme file from the themes folder that overrides colors, shapes, fonts or "
                                    "motion. Themes are data only and are reloaded when they change."),
                    .flags = flag::Advanced});
    defs.push_back({.key = keys::EnforceContrast,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Enforce readable contrast"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Adjust colors that would fall below WCAG AA contrast against glass.")});

    // --- Appearance: accent ----------------------------------------------------------------------
    defs.push_back(
        {.key = keys::AccentAdaptive,
         .type = Type::Bool,
         .defaultValue = true,
         .category = Appearance,
         .label = QT_TRANSLATE_NOOP("Settings", "Adapt accent colors to cover art"),
         .description = QT_TRANSLATE_NOOP("Settings", "Derive the accent palette from the current track's artwork.")});
    defs.push_back({.key = keys::AccentColor,
                    .type = Type::Color,
                    .defaultValue = str("#6774f9"),
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Accent color"),
                    .description = QT_TRANSLATE_NOOP("Settings",
                                                     "Seed color for the whole palette when adaptive accents are off."),
                    .visibleWhen = Condition{keys::AccentAdaptive, {"false"}}});
    defs.push_back({.key = keys::AccentExtraction,
                    .type = Type::Enum,
                    .defaultValue = str("vivid"),
                    .options = {{"vivid", QT_TRANSLATE_NOOP("Settings", "Vivid")},
                                {"balanced", QT_TRANSLATE_NOOP("Settings", "Balanced")},
                                {"soft", QT_TRANSLATE_NOOP("Settings", "Soft")}},
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Palette extraction"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "How saturated the colours taken from the artwork are. Recomputed on every track."),
                    .visibleWhen = Condition{keys::AccentAdaptive, {"true"}}});
    defs.push_back(
        {.key = keys::BackdropStyle,
         .type = Type::Enum,
         .defaultValue = str("aurora"),
         .options = {{"aurora", QT_TRANSLATE_NOOP("Settings", "Aurora")},
                     {"cover", QT_TRANSLATE_NOOP("Settings", "Cover art")},
                     {"dark", QT_TRANSLATE_NOOP("Settings", "Dark")}},
         .category = Appearance,
         .label = QT_TRANSLATE_NOOP("Settings", "Backdrop style"),
         .description = QT_TRANSLATE_NOOP(
             "Settings",
             "Aurora: soft light in the palette colours. Cover art: the blurred artwork. Dark: nearly plain.")});
    defs.push_back({.key = keys::BackdropAnimate,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Animate backdrop"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Slow drifting motion. Paused automatically when the window is hidden.")});

    // --- Appearance: glow ------------------------------------------------------------------------
    defs.push_back({.key = keys::GlowIntensity,
                    .type = Type::Real,
                    .defaultValue = real(0.3),
                    .min = 0.0,
                    .max = 1.0,
                    .step = 0.05,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Glow intensity"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Strength of the glow and of the backdrop light.")});
    defs.push_back({.key = keys::GlowCover,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Artwork glow"),
                    .description = QT_TRANSLATE_NOOP("Settings", "A shadow in the artwork's colour under covers.")});
    defs.push_back(
        {.key = keys::GlowControls,
         .type = Type::Bool,
         .defaultValue = true,
         .category = Appearance,
         .label = QT_TRANSLATE_NOOP("Settings", "Button and selection glow"),
         .description = QT_TRANSLATE_NOOP("Settings", "A soft halo around accented buttons and active items.")});
    defs.push_back(
        {.key = keys::GlowProgress,
         .type = Type::Bool,
         .defaultValue = true,
         .category = Appearance,
         .label = QT_TRANSLATE_NOOP("Settings", "Progress glow"),
         .description = QT_TRANSLATE_NOOP("Settings", "Lights up the played part of the progress bar and waveform.")});

    // --- Appearance: glass -----------------------------------------------------------------------
    defs.push_back({.key = keys::GlassStyle,
                    .type = Type::Enum,
                    .defaultValue = str("kawase"),
                    .options = {{"kawase", QT_TRANSLATE_NOOP("Settings", "Dual Kawase")},
                                {"gaussian", QT_TRANSLATE_NOOP("Settings", "Gaussian")},
                                {"solid", QT_TRANSLATE_NOOP("Settings", "None (opaque)")}},
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Backdrop blur"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "How the backdrop behind surfaces is blurred. Blurs need hardware acceleration; "
                                    "None is the cheapest. Works with every surface style.")});
    defs.push_back({.key = keys::GlassSurface,
                    .type = Type::Enum,
                    .defaultValue = str("frosted"),
                    .options = {{"frosted", QT_TRANSLATE_NOOP("Settings", "Frosted glass")},
                                {"liquid", QT_TRANSLATE_NOOP("Settings", "Liquid Glass")},
                                {"flat", QT_TRANSLATE_NOOP("Settings", "Flat")}},
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Surface style"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Frosted: lit edges, sheen and depth. Liquid Glass: refractive edges (needs a "
                                    "blur). Flat: the blurred backdrop and a tint, without highlights or shadows.")});
    defs.push_back({.key = keys::GlassBlurStrength,
                    .type = Type::Real,
                    .defaultValue = real(0.6),
                    .min = 0.0,
                    .max = 1.0,
                    .step = 0.05,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Blur strength"),
                    .description = QT_TRANSLATE_NOOP("Settings", "How strongly the backdrop is blurred."),
                    .visibleWhen = Condition{keys::GlassStyle, {"kawase", "gaussian"}}});
    defs.push_back({.key = keys::GlassOpacity,
                    .type = Type::Real,
                    .defaultValue = real(0.0),
                    .min = 0.0,
                    .max = 1.0,
                    .step = 0.05,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Glass tint"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Opacity of the tint applied to glass surfaces.")});
    defs.push_back({.key = keys::GlassGrain,
                    .type = Type::Real,
                    .defaultValue = real(0.0),
                    .min = 0.0,
                    .max = 1.0,
                    .step = 0.05,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Grain"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Fine noise that prevents banding in gradients.")});
    defs.push_back({.key = keys::GlassQuality,
                    .type = Type::Enum,
                    .defaultValue = str("auto"),
                    .options = {{"auto", QT_TRANSLATE_NOOP("Settings", "Auto")},
                                {"low", QT_TRANSLATE_NOOP("Settings", "Low")},
                                {"medium", QT_TRANSLATE_NOOP("Settings", "Medium")},
                                {"high", QT_TRANSLATE_NOOP("Settings", "High")}},
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Effects quality"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Auto lowers quality when frames take too long and raises it again later.")});
    defs.push_back(
        {.key = keys::ButtonMaterial,
         .type = Type::Enum,
         .defaultValue = str("frosted"),
         .options = {{"frosted", QT_TRANSLATE_NOOP("Settings", "Frosted glass")},
                     {"liquid", QT_TRANSLATE_NOOP("Settings", "Liquid glass")},
                     {"tinted", QT_TRANSLATE_NOOP("Settings", "Tinted glass")},
                     {"plain", QT_TRANSLATE_NOOP("Settings", "Plain")},
                     {"clear", QT_TRANSLATE_NOOP("Settings", "Clear")},
                     {"solid", QT_TRANSLATE_NOOP("Settings", "Solid")}},
         .category = Appearance,
         .label = QT_TRANSLATE_NOOP("Settings", "Button material"),
         .description = QT_TRANSLATE_NOOP(
             "Settings", "Frosted: milky blurred glass. Liquid: glossy glass with a bright rim. Tinted: glass coloured "
                         "by the accent. Plain: a flat translucent fill. Clear: outline only. Solid: opaque.")});
    defs.push_back({.key = keys::PrimaryButtons,
                    .type = Type::Enum,
                    .defaultValue = str("filled"),
                    .options = {{"filled", QT_TRANSLATE_NOOP("Settings", "Filled accent")},
                                {"material", QT_TRANSLATE_NOOP("Settings", "Like other buttons")},
                                {"outline", QT_TRANSLATE_NOOP("Settings", "Accent outline")},
                                {"gradient", QT_TRANSLATE_NOOP("Settings", "Gradient glass")}},
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Main buttons"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Play and other main actions. Filled: a flat accent shape. Like other buttons: "
                                    "the button material with an accent icon. Outline: an accent ring. Gradient: "
                                    "glossy accent glass.")});
    defs.push_back({.key = keys::ReduceTransparency,
                    .type = Type::Bool,
                    .defaultValue = false,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Reduce transparency"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Use opaque surfaces everywhere.")});

    // --- Appearance: motion ----------------------------------------------------------------------
    defs.push_back({.key = keys::ReduceMotion,
                    .type = Type::Bool,
                    .defaultValue = false,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Reduce motion"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Replace movement with simple fades and stop the drifting backdrop.")});
    defs.push_back({.key = keys::AnimationSpeed,
                    .type = Type::Real,
                    .defaultValue = real(1.0),
                    .min = 0.0,
                    .max = 3.0,
                    .step = 0.25,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Animation speed"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Duration multiplier. 0 turns animations off."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "×")});

    // --- Appearance: shape -----------------------------------------------------------------------
    defs.push_back(
        {.key = keys::ColorTransition,
         .type = Type::Enum,
         .defaultValue = str("smooth"),
         .options = {{"fast", QT_TRANSLATE_NOOP("Settings", "Fast")},
                     {"smooth", QT_TRANSLATE_NOOP("Settings", "Smooth")},
                     {"slow", QT_TRANSLATE_NOOP("Settings", "Slow")}},
         .category = Appearance,
         .label = QT_TRANSLATE_NOOP("Settings", "Colour transitions"),
         .description = QT_TRANSLATE_NOOP(
             "Settings", "How quickly colours blend into the new palette when the track or theme changes.")});
    defs.push_back({.key = keys::CornerRadius,
                    .type = Type::Enum,
                    .defaultValue = str("default"),
                    .options = {{"sharp", QT_TRANSLATE_NOOP("Settings", "Sharp")},
                                {"default", QT_TRANSLATE_NOOP("Settings", "Default")},
                                {"round", QT_TRANSLATE_NOOP("Settings", "Round")},
                                {"custom", QT_TRANSLATE_NOOP("Settings", "Custom")}},
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Corner radius"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Rounding of panels, cards and buttons.")});
    defs.push_back({.key = keys::CornerRadiusMultiplier,
                    .type = Type::Real,
                    .defaultValue = real(1.0),
                    .min = 0.0,
                    .max = 2.0,
                    .step = 0.05,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Corner radius multiplier"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Scales every corner radius."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "×"),
                    .visibleWhen = Condition{keys::CornerRadius, {"custom"}}});
    defs.push_back({.key = keys::ButtonShape,
                    .type = Type::Enum,
                    .defaultValue = str("pill"),
                    .options = {{"pill", QT_TRANSLATE_NOOP("Settings", "Pill")},
                                {"soft", QT_TRANSLATE_NOOP("Settings", "Soft")},
                                {"sharp", QT_TRANSLATE_NOOP("Settings", "Sharp")}},
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Button shape"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Corner style of buttons and segmented controls.")});
    defs.push_back({.key = keys::CoverArtShape,
                    .type = Type::Enum,
                    .defaultValue = str("rounded"),
                    .options = {{"rounded", QT_TRANSLATE_NOOP("Settings", "Rounded")},
                                {"square", QT_TRANSLATE_NOOP("Settings", "Square")},
                                {"circle", QT_TRANSLATE_NOOP("Settings", "Circle")}},
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Cover art shape"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Shape of artwork in lists and the player.")});

    // --- Appearance: text and scale --------------------------------------------------------------
    defs.push_back({.key = keys::UiScale,
                    .type = Type::Int,
                    .defaultValue = integer(100),
                    .min = 75,
                    .max = 200,
                    .step = 5,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Interface zoom"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Scales the whole interface."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "%")});
    defs.push_back({.key = keys::FontScale,
                    .type = Type::Int,
                    .defaultValue = integer(100),
                    .min = 80,
                    .max = 150,
                    .step = 5,
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Text size"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Scales text relative to the interface zoom."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "%")});
    defs.push_back({.key = keys::HeadingFont,
                    .type = Type::String,
                    .defaultValue = str(""),
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Heading font"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Font family for titles. Empty uses Manrope."),
                    .flags = flag::Advanced});
    defs.push_back({.key = keys::BodyFont,
                    .type = Type::String,
                    .defaultValue = str(""),
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Body font"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Font family for interface text. Empty uses Inter."),
                    .flags = flag::Advanced});
    defs.push_back({.key = keys::MonoFont,
                    .type = Type::String,
                    .defaultValue = str(""),
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Monospace font"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Font family for timings and technical details. Empty uses JetBrains Mono."),
                    .flags = flag::Advanced});
    defs.push_back({.key = keys::IconWeight,
                    .type = Type::Enum,
                    .defaultValue = str("regular"),
                    .options = {{"light", QT_TRANSLATE_NOOP("Settings", "Light")},
                                {"regular", QT_TRANSLATE_NOOP("Settings", "Regular")},
                                {"bold", QT_TRANSLATE_NOOP("Settings", "Bold")}},
                    .category = Appearance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Icon weight"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Stroke thickness of icons.")});

    // --- Layout ----------------------------------------------------------------------------------
    defs.push_back({.key = keys::Density,
                    .type = Type::Enum,
                    .defaultValue = str("cozy"),
                    .options = {{"compact", QT_TRANSLATE_NOOP("Settings", "Compact")},
                                {"cozy", QT_TRANSLATE_NOOP("Settings", "Comfortable")},
                                {"airy", QT_TRANSLATE_NOOP("Settings", "Spacious")}},
                    .category = Layout,
                    .label = QT_TRANSLATE_NOOP("Settings", "Density"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Spacing between sections, cards and rows.")});

    // --- Playback -------------------------------------------------------------------------------
    defs.push_back({.key = keys::StreamQuality,
                    .type = Type::Enum,
                    .defaultValue = str("auto"),
                    .options = {{"auto", QT_TRANSLATE_NOOP("Settings", "Auto")},
                                {"best", QT_TRANSLATE_NOOP("Settings", "Best available")},
                                {"data_saver", QT_TRANSLATE_NOOP("Settings", "Data saver")}},
                    .category = Playback,
                    .label = QT_TRANSLATE_NOOP("Settings", "Audio quality"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Auto plays the best stream your account may play, and the smallest one on a "
                                    "metered connection. Applies from the next track.")});
    defs.push_back({.key = keys::Crossfade,
                    .type = Type::Int,
                    .defaultValue = integer(0),
                    .min = 0,
                    .max = 12,
                    .step = 1,
                    .category = Playback,
                    .label = QT_TRANSLATE_NOOP("Settings", "Crossfade"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Blends the end of a track into the next one. 0 plays them back to back without "
                                    "a gap."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "s")});
    defs.push_back({.key = keys::RememberVolume,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Playback,
                    .label = QT_TRANSLATE_NOOP("Settings", "Remember volume"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Start with the volume you last used.")});
    defs.push_back({.key = keys::StartupVolume,
                    .type = Type::Int,
                    .defaultValue = integer(70),
                    .min = 0,
                    .max = 100,
                    .step = 5,
                    .category = Playback,
                    .label = QT_TRANSLATE_NOOP("Settings", "Volume at start"),
                    .description = QT_TRANSLATE_NOOP("Settings", "The volume every session starts with."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "%"),
                    .visibleWhen = Condition{keys::RememberVolume, {"false"}}});
    defs.push_back({.key = keys::Autoplay,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Playback,
                    .label = QT_TRANSLATE_NOOP("Settings", "Autoplay related tracks"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "When the queue ends, keep playing tracks related to the last one. Repeat "
                                    "takes precedence.")});
    defs.push_back({.key = keys::RestoreQueue,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Playback,
                    .label = QT_TRANSLATE_NOOP("Settings", "Restore the queue"),
                    .description = QT_TRANSLATE_NOOP("Settings",
                                                     "Open with the queue, track and position of last time, paused.")});
    defs.push_back({.key = keys::MediaControls,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Playback,
                    .label = QT_TRANSLATE_NOOP("Settings", "Media keys and system controls"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Lets media keys, the desktop's media controls and other apps control playback "
                                    "and show the current track (MPRIS).")});
    defs.push_back({.key = keys::TrayIcon,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Playback,
                    .label = QT_TRANSLATE_NOOP("Settings", "Tray icon"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Playback controls in the system tray, where the desktop provides one.")});
    defs.push_back({.key = keys::TrackNotifications,
                    .type = Type::Bool,
                    .defaultValue = false,
                    .category = Playback,
                    .label = QT_TRANSLATE_NOOP("Settings", "Notify on track change"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Shows a desktop notification with the next track while Noctara's window is "
                                    "not in front.")});
    defs.push_back({.key = keys::ProtectedEnabled,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Playback,
                    .label = QT_TRANSLATE_NOOP("Settings", "Play protected tracks"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Plays encrypted tracks through the protected player, which starts only when "
                                    "one plays. Off skips them.")});
    defs.push_back({.key = keys::ProtectedAutoInstall,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Playback,
                    .label = QT_TRANSLATE_NOOP("Settings", "Set up the protected player automatically"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Downloads it once (castlabs Electron with Widevine, about 125 MB) when it is "
                                    "missing, in the background and not on a metered connection."),
                    .flags = flag::Advanced,
                    .visibleWhen = Condition{keys::ProtectedEnabled, {"true"}}});
    defs.push_back({.key = keys::ProtectedHelper,
                    .type = Type::Path,
                    .defaultValue = str(""),
                    .category = Playback,
                    .label = QT_TRANSLATE_NOOP("Settings", "Protected player folder"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Folder with a protected player of your own (as made by `xmake drm-helper`). "
                                    "Empty uses one next to Noctara, in its install directory, or the one Noctara "
                                    "set up."),
                    .flags = flag::Advanced,
                    .visibleWhen = Condition{keys::ProtectedEnabled, {"true"}}});
    defs.push_back({.key = keys::ProtectedIdle,
                    .type = Type::Int,
                    .defaultValue = integer(5),
                    .min = 1,
                    .max = 60,
                    .step = 1,
                    .category = Playback,
                    .label = QT_TRANSLATE_NOOP("Settings", "Close the protected player after"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Frees its memory when no protected track has played for this long. A paused "
                                    "track resumes where it was."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "min"),
                    .flags = flag::Advanced,
                    .visibleWhen = Condition{keys::ProtectedEnabled, {"true"}}});
    defs.push_back({.key = keys::ProtectedGpu,
                    .type = Type::Bool,
                    .defaultValue = false,
                    .category = Playback,
                    .label = QT_TRANSLATE_NOOP("Settings", "Protected player uses the GPU"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "It only plays audio and never needs the GPU. Applies when it next starts."),
                    .flags = flag::Advanced,
                    .visibleWhen = Condition{keys::ProtectedEnabled, {"true"}}});

    defs.push_back({.key = keys::NavPosition,
                    .type = Type::Enum,
                    .defaultValue = str("left"),
                    .options = {{"left", QT_TRANSLATE_NOOP("Settings", "Left")},
                                {"right", QT_TRANSLATE_NOOP("Settings", "Right")},
                                {"bottom", QT_TRANSLATE_NOOP("Settings", "Bottom")}},
                    .category = Layout,
                    .label = QT_TRANSLATE_NOOP("Settings", "Position"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Which edge of the window the navigation sits on.")});
    defs.push_back({.key = keys::NavMode,
                    .type = Type::Enum,
                    .defaultValue = str("compact"),
                    .options = {{"compact", QT_TRANSLATE_NOOP("Settings", "Icons")},
                                {"expanded", QT_TRANSLATE_NOOP("Settings", "Icons and labels")},
                                {"auto_hide", QT_TRANSLATE_NOOP("Settings", "Auto-hide")}},
                    .category = Layout,
                    .label = QT_TRANSLATE_NOOP("Settings", "Style"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Auto-hide shows the navigation when the pointer reaches its edge.")});
    defs.push_back(
        {.key = keys::NavItems,
         .type = Type::StringList,
         .defaultValue = QVariant::fromValue(QStringList{QStringLiteral("home"), QStringLiteral("search"),
                                                         QStringLiteral("library"), QStringLiteral("likes"),
                                                         QStringLiteral("history"), QStringLiteral("charts")}),
         .category = Layout,
         .label = QT_TRANSLATE_NOOP("Settings", "Pages in the navigation"),
         .description = QT_TRANSLATE_NOOP("Settings", "Their order and which are shown. Settings is always there."),
         .validator = validateNavItems});
    defs.push_back({.key = keys::PlayerPosition,
                    .type = Type::Enum,
                    .defaultValue = str("bottom"),
                    .options = {{"bottom", QT_TRANSLATE_NOOP("Settings", "Bottom")},
                                {"top", QT_TRANSLATE_NOOP("Settings", "Top")}},
                    .category = Layout,
                    .label = QT_TRANSLATE_NOOP("Settings", "Position"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Where the player bar floats.")});
    defs.push_back({.key = keys::PlayerSize,
                    .type = Type::Enum,
                    .defaultValue = str("regular"),
                    .options = {{"compact", QT_TRANSLATE_NOOP("Settings", "Compact")},
                                {"regular", QT_TRANSLATE_NOOP("Settings", "Regular")},
                                {"large", QT_TRANSLATE_NOOP("Settings", "Large")}},
                    .category = Layout,
                    .label = QT_TRANSLATE_NOOP("Settings", "Size"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Height of the player bar and its cover.")});
    defs.push_back({.key = keys::PlayerWaveform,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Layout,
                    .label = QT_TRANSLATE_NOOP("Settings", "Waveform"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Off shows a plain progress line instead.")});
    defs.push_back({.key = keys::PlayerTime,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Layout,
                    .label = QT_TRANSLATE_NOOP("Settings", "Elapsed and total time"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Times on both sides of the waveform.")});
    defs.push_back({.key = keys::PlayerVolume,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Layout,
                    .label = QT_TRANSLATE_NOOP("Settings", "Volume"),
                    .description = QT_TRANSLATE_NOOP("Settings",
                                                     "The volume slider (narrow windows show only the mute button).")});
    defs.push_back({.key = keys::PlayerQueueButton,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Layout,
                    .label = QT_TRANSLATE_NOOP("Settings", "Queue button"),
                    .description = QT_TRANSLATE_NOOP("Settings", "The queue also opens with its shortcut.")});
    for (const auto& [key, label, fallback] :
         {std::tuple{keys::ViewLibrary, QT_TRANSLATE_NOOP("Settings", "Library"), "grid"},
          std::tuple{keys::ViewLikes, QT_TRANSLATE_NOOP("Settings", "Likes"), "list"},
          std::tuple{keys::ViewHistory, QT_TRANSLATE_NOOP("Settings", "History"), "list"},
          std::tuple{keys::ViewCharts, QT_TRANSLATE_NOOP("Settings", "Charts"), "list"}}) {
        defs.push_back({.key = key,
                        .type = Type::Enum,
                        .defaultValue = str(fallback),
                        .options = {{"list", QT_TRANSLATE_NOOP("Settings", "List")},
                                    {"grid", QT_TRANSLATE_NOOP("Settings", "Grid")}},
                        .category = Layout,
                        .label = label,
                        .description = QT_TRANSLATE_NOOP("Settings", "How the page shows its items at first; the "
                                                                     "page's own switch changes it too.")});
    }
    defs.push_back({.key = keys::CardSize,
                    .type = Type::Enum,
                    .defaultValue = str("medium"),
                    .options = {{"small", QT_TRANSLATE_NOOP("Settings", "Small")},
                                {"medium", QT_TRANSLATE_NOOP("Settings", "Medium")},
                                {"large", QT_TRANSLATE_NOOP("Settings", "Large")}},
                    .category = Layout,
                    .label = QT_TRANSLATE_NOOP("Settings", "Card size"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Grids fit as many cards of this size per row as the window allows.")});
    for (const auto& [key, label, description, fallback] :
         {std::tuple{keys::RowPlays, QT_TRANSLATE_NOOP("Settings", "Plays"),
                     QT_TRANSLATE_NOOP("Settings", "How often a track was played."), true},
          std::tuple{keys::RowLikes, QT_TRANSLATE_NOOP("Settings", "Likes"),
                     QT_TRANSLATE_NOOP("Settings", "How many people liked a track."), false},
          std::tuple{keys::RowReposts, QT_TRANSLATE_NOOP("Settings", "Reposts"),
                     QT_TRANSLATE_NOOP("Settings", "How often a track was reposted."), false},
          std::tuple{keys::RowDuration, QT_TRANSLATE_NOOP("Settings", "Duration"),
                     QT_TRANSLATE_NOOP("Settings", "The length of a track or playlist."), true}}) {
        defs.push_back({.key = key,
                        .type = Type::Bool,
                        .defaultValue = fallback,
                        .category = Layout,
                        .label = label,
                        .description = description});
    }
    defs.push_back({.key = keys::FullPlayerArt,
                    .type = Type::Enum,
                    .defaultValue = str("large"),
                    .options = {{"medium", QT_TRANSLATE_NOOP("Settings", "Medium")},
                                {"large", QT_TRANSLATE_NOOP("Settings", "Large")}},
                    .category = Layout,
                    .label = QT_TRANSLATE_NOOP("Settings", "Cover size"),
                    .description = QT_TRANSLATE_NOOP("Settings", "How much of the player the cover art takes.")});
    defs.push_back({.key = keys::FullPlayerQueue,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Layout,
                    .label = QT_TRANSLATE_NOOP("Settings", "Queue beside the cover"),
                    .description = QT_TRANSLATE_NOOP("Settings", "When the window is wide enough.")});
    defs.push_back({.key = keys::FullPlayerWaveform,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Layout,
                    .label = QT_TRANSLATE_NOOP("Settings", "Waveform"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Off shows a plain progress line instead.")});
    defs.push_back({.key = keys::WindowDecorations,
                    .type = Type::Enum,
                    .defaultValue = str("native"),
                    .options = {{"native", QT_TRANSLATE_NOOP("Settings", "System")},
                                {"custom", QT_TRANSLATE_NOOP("Settings", "Noctara")}},
                    .category = Layout,
                    .label = QT_TRANSLATE_NOOP("Settings", "Window frame"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Noctara draws its own title bar and buttons in the glass style. Window "
                                    "managers without decorations keep showing none."),
                    .flags = flag::RequiresRestart});
    defs.push_back({.key = keys::RememberGeometry,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Layout,
                    .label = QT_TRANSLATE_NOOP("Settings", "Remember window size"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Open with the size (and, where the system allows, the position) of last "
                                    "time.")});
    defs.push_back({.key = keys::StartPage,
                    .type = Type::Enum,
                    .defaultValue = str("home"),
                    .options = {{"home", QT_TRANSLATE_NOOP("Settings", "Home")},
                                {"search", QT_TRANSLATE_NOOP("Settings", "Search")},
                                {"library", QT_TRANSLATE_NOOP("Settings", "Library")},
                                {"likes", QT_TRANSLATE_NOOP("Settings", "Likes")},
                                {"history", QT_TRANSLATE_NOOP("Settings", "History")},
                                {"charts", QT_TRANSLATE_NOOP("Settings", "Charts")},
                                {"last", QT_TRANSLATE_NOOP("Settings", "Where I left off")}},
                    .category = Layout,
                    .label = QT_TRANSLATE_NOOP("Settings", "Start page"),
                    .description = QT_TRANSLATE_NOOP("Settings", "The page shown when Noctara opens.")});
    defs.push_back(
        {.key = keys::StartMinimized,
         .type = Type::Bool,
         .defaultValue = false,
         .category = Layout,
         .label = QT_TRANSLATE_NOOP("Settings", "Start minimized"),
         .description = QT_TRANSLATE_NOOP("Settings", "Start in the tray when there is one, otherwise minimized.")});
    defs.push_back({.key = keys::CloseToTray,
                    .type = Type::Bool,
                    .defaultValue = false,
                    .category = Layout,
                    .label = QT_TRANSLATE_NOOP("Settings", "Keep playing when closed"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Closing the window hides it in the tray; Quit in the tray menu ends Noctara. "
                                    "Needs the tray icon.")});

    // --- Keyboard --------------------------------------------------------------------------------
    // The action is the last part of the key (see ui::KeyboardController).
    for (const auto& [key, label, sequence] :
         {std::tuple{"keyboard.shortcuts.play_pause", QT_TRANSLATE_NOOP("Settings", "Play or pause"), "Space"},
          std::tuple{"keyboard.shortcuts.next", QT_TRANSLATE_NOOP("Settings", "Next track"), "Ctrl+Right"},
          std::tuple{"keyboard.shortcuts.previous", QT_TRANSLATE_NOOP("Settings", "Previous track"), "Ctrl+Left"},
          std::tuple{"keyboard.shortcuts.seek_forward", QT_TRANSLATE_NOOP("Settings", "Forward 5 seconds"),
                     "Shift+Right"},
          std::tuple{"keyboard.shortcuts.seek_backward", QT_TRANSLATE_NOOP("Settings", "Back 5 seconds"), "Shift+Left"},
          std::tuple{"keyboard.shortcuts.volume_up", QT_TRANSLATE_NOOP("Settings", "Volume up"), "Ctrl+Up"},
          std::tuple{"keyboard.shortcuts.volume_down", QT_TRANSLATE_NOOP("Settings", "Volume down"), "Ctrl+Down"},
          std::tuple{"keyboard.shortcuts.mute", QT_TRANSLATE_NOOP("Settings", "Mute"), "Ctrl+M"},
          std::tuple{"keyboard.shortcuts.shuffle", QT_TRANSLATE_NOOP("Settings", "Shuffle"), "Ctrl+S"},
          std::tuple{"keyboard.shortcuts.repeat", QT_TRANSLATE_NOOP("Settings", "Repeat"), "Ctrl+R"},
          std::tuple{"keyboard.shortcuts.queue", QT_TRANSLATE_NOOP("Settings", "Show the queue"), "Ctrl+Shift+Q"},
          std::tuple{"keyboard.shortcuts.full_player", QT_TRANSLATE_NOOP("Settings", "Full-screen player"),
                     "Ctrl+Shift+F"},
          std::tuple{"keyboard.shortcuts.search", QT_TRANSLATE_NOOP("Settings", "Search"), "Ctrl+K"},
          std::tuple{"keyboard.shortcuts.back", QT_TRANSLATE_NOOP("Settings", "Back"), "Alt+Left"},
          std::tuple{"keyboard.shortcuts.settings", QT_TRANSLATE_NOOP("Settings", "Settings"), "Ctrl+,"},
          std::tuple{"keyboard.shortcuts.appearance", QT_TRANSLATE_NOOP("Settings", "Appearance panel"),
                     "Ctrl+Shift+A"},
          std::tuple{"keyboard.shortcuts.cheat_sheet", QT_TRANSLATE_NOOP("Settings", "Show all shortcuts"),
                     "Ctrl+/"}}) {
        defs.push_back({.key = key,
                        .type = Type::Shortcut,
                        .defaultValue = str(sequence),
                        .category = Keyboard,
                        .label = label,
                        .validator = validateShortcut});
    }
    defs.push_back({.key = keys::GlobalShortcuts,
                    .type = Type::Bool,
                    .defaultValue = false,
                    .category = Keyboard,
                    .label = QT_TRANSLATE_NOOP("Settings", "Global playback shortcuts"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Registers play/pause, next, previous and “show Noctara” with the desktop "
                                    "(XDG global shortcuts portal), which asks you for the keys. Media keys work "
                                    "without this.")});

    // --- Performance -----------------------------------------------------------------------------
    defs.push_back(
        {.key = keys::GpuAcceleration,
         .type = Type::Enum,
         .defaultValue = str("auto"),
         .options = {{"auto", QT_TRANSLATE_NOOP("Settings", "Auto")},
                     {"on", QT_TRANSLATE_NOOP("Settings", "On")},
                     {"off", QT_TRANSLATE_NOOP("Settings", "Off")}},
         .category = Performance,
         .label = QT_TRANSLATE_NOOP("Settings", "Hardware acceleration"),
         .description = QT_TRANSLATE_NOOP(
             "Settings", "Auto uses the GPU unless it is missing, a software rasterizer or known to be unreliable. "
                         "Off renders on the CPU and disables blur effects."),
         .flags = flag::RequiresRestart,
         .cliAlias = "gpu"});
    defs.push_back({.key = keys::GraphicsApi,
                    .type = Type::Enum,
                    .defaultValue = str("auto"),
                    .options = {{"auto", QT_TRANSLATE_NOOP("Settings", "Auto")},
                                {"vulkan", "Vulkan", false},
                                {"opengl", "OpenGL", false}},
                    .category = Performance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Graphics API"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Rendering backend used for hardware acceleration."),
                    .flags = flag::RequiresRestart,
                    .visibleWhen = Condition{keys::GpuAcceleration, {"auto", "on"}},
                    .cliAlias = "graphics-api"});
    defs.push_back({.key = keys::GpuPreference,
                    .type = Type::Enum,
                    .defaultValue = str("auto"),
                    .options = {{"auto", QT_TRANSLATE_NOOP("Settings", "Auto")},
                                {"integrated", QT_TRANSLATE_NOOP("Settings", "Integrated (power saving)")},
                                {"discrete", QT_TRANSLATE_NOOP("Settings", "Discrete (high performance)")}},
                    .category = Performance,
                    .label = QT_TRANSLATE_NOOP("Settings", "GPU preference"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Which GPU to use on systems with more than one."),
                    .flags = flag::RequiresRestart | flag::Advanced,
                    .visibleWhen = Condition{keys::GpuAcceleration, {"auto", "on"}}});
    defs.push_back(
        {.key = keys::VSync,
         .type = Type::Bool,
         .defaultValue = true,
         .category = Performance,
         .label = QT_TRANSLATE_NOOP("Settings", "Vertical sync"),
         .description = QT_TRANSLATE_NOOP("Settings", "Synchronise frames with the display to avoid tearing."),
         .flags = flag::RequiresRestart | flag::Advanced});
    defs.push_back({.key = keys::FrameRateCap,
                    .type = Type::Enum,
                    .defaultValue = str("auto"),
                    .options = {{"auto", QT_TRANSLATE_NOOP("Settings", "Display refresh rate")},
                                {"30", "30", false},
                                {"60", "60", false},
                                {"120", "120", false},
                                {"144", "144", false},
                                {"unlimited", QT_TRANSLATE_NOOP("Settings", "Unlimited")}},
                    .category = Performance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Frame rate limit"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Upper bound for animation frame rate."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "FPS"),
                    .flags = flag::Advanced});
    defs.push_back({.key = keys::EnergySaving,
                    .type = Type::Enum,
                    .defaultValue = str("auto"),
                    .options = {{"auto", QT_TRANSLATE_NOOP("Settings", "Automatic (on battery)")},
                                {"always", QT_TRANSLATE_NOOP("Settings", "Always")},
                                {"off", QT_TRANSLATE_NOOP("Settings", "Off")}},
                    .category = Performance,
                    .label = QT_TRANSLATE_NOOP("Settings", "Energy saving"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings",
                        "While active, Noctara pauses the animated backdrop, uses the lighter Dual Kawase blur instead "
                        "of Liquid Glass or Gaussian, and limits animations to 30 frames per second. Automatic turns "
                        "it on only while the computer runs on battery.")});

    // --- Network: data usage -----------------------------------------------------------------------
    defs.push_back({.key = keys::OfflineMode,
                    .type = Type::Bool,
                    .defaultValue = false,
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "Offline mode"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Make no network connections at all. Only content that is already cached is "
                                    "available."),
                    .cliAlias = "offline"});
    defs.push_back({.key = keys::MeteredMode,
                    .type = Type::Enum,
                    .defaultValue = str("auto"),
                    .options = {{"auto", QT_TRANSLATE_NOOP("Settings", "Auto")},
                                {"on", QT_TRANSLATE_NOOP("Settings", "Always")},
                                {"off", QT_TRANSLATE_NOOP("Settings", "Never")}},
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "Metered connection"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "On a metered connection Noctara skips prefetching and other background "
                                    "downloads. Auto follows the system's setting for the current network.")});
    defs.push_back(
        {.key = keys::BandwidthLimit,
         .type = Type::Int,
         .defaultValue = integer(0),
         .min = 0,
         .max = 1000,
         .step = 1,
         .category = Network,
         .label = QT_TRANSLATE_NOOP("Settings", "Download speed limit"),
         .description = QT_TRANSLATE_NOOP("Settings", "Upper bound for all downloads together. 0 means no limit."),
         .unit = QT_TRANSLATE_NOOP("Settings", "Mbit/s"),
         .flags = flag::Advanced});

    // --- Network: DNS ------------------------------------------------------------------------------
    defs.push_back({.key = keys::DnsMode,
                    .type = Type::Enum,
                    .defaultValue = str("system"),
                    .options = {{"system", QT_TRANSLATE_NOOP("Settings", "System resolver")},
                                {"custom", QT_TRANSLATE_NOOP("Settings", "Custom DNS servers")},
                                {"doh", QT_TRANSLATE_NOOP("Settings", "DNS over HTTPS")}},
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "DNS mode"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "System uses your computer's settings. DNS over HTTPS encrypts lookups so the "
                                    "network cannot see or change them."),
                    .cliAlias = "dns"});
    defs.push_back({.key = keys::DnsServerPreset,
                    .type = Type::Enum,
                    .defaultValue = str("cloudflare"),
                    .options = {{"cloudflare", "Cloudflare", false},
                                {"google", "Google", false},
                                {"quad9", "Quad9", false},
                                {"adguard", "AdGuard", false},
                                {"custom", QT_TRANSLATE_NOOP("Settings", "Custom")}},
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "DNS servers"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Asked in order; the next one is tried when a server does not answer."),
                    .visibleWhen = Condition{keys::DnsMode, {"custom"}}});
    defs.push_back({.key = keys::DnsServers,
                    .type = Type::StringList,
                    .defaultValue = QVariant::fromValue(QStringList{}),
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "Server addresses"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "IPv4 or IPv6 addresses, optionally with a port: 192.0.2.1, [2001:db8::1]:5353."),
                    .visibleWhen = Condition{keys::DnsServerPreset, {"custom"}},
                    .validator = validateDnsServers});
    defs.push_back({.key = keys::DohProvider,
                    .type = Type::Enum,
                    .defaultValue = str("cloudflare"),
                    .options = {{"cloudflare", "Cloudflare", false},
                                {"google", "Google", false},
                                {"quad9", "Quad9", false},
                                {"adguard", "AdGuard", false},
                                {"mullvad", "Mullvad", false},
                                {"custom", QT_TRANSLATE_NOOP("Settings", "Custom")}},
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "DNS-over-HTTPS provider"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Custom accepts any RFC 8484 server, for example a personal NextDNS address."),
                    .visibleWhen = Condition{keys::DnsMode, {"doh"}}});
    defs.push_back({.key = keys::DohUrl,
                    .type = Type::Url,
                    .defaultValue = str(""),
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "DoH address"),
                    .description = QT_TRANSLATE_NOOP("Settings", "For example https://dns.example/dns-query."),
                    .visibleWhen = Condition{keys::DohProvider, {"custom"}},
                    .validator = validateDohUrl});
    defs.push_back({.key = keys::DohBootstrap,
                    .type = Type::StringList,
                    .defaultValue = QVariant::fromValue(QStringList{}),
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "Bootstrap addresses"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "IP addresses of the DoH server, so its own name never has to be looked up "
                                    "elsewhere. Required when fallback to the system resolver is off."),
                    .visibleWhen = Condition{keys::DohProvider, {"custom"}},
                    .validator = validateAddresses});
    defs.push_back({.key = keys::DohAllowInsecure,
                    .type = Type::Bool,
                    .defaultValue = false,
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "Allow insecure DoH"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Accept a plain http:// address, but only for a server on this computer or the "
                                    "local network."),
                    .flags = flag::Advanced,
                    .visibleWhen = Condition{keys::DohProvider, {"custom"}}});
    defs.push_back({.key = keys::DnsFallbackToSystem,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "Fall back to the system resolver"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Keeps Noctara working when the chosen servers are unreachable, but those "
                                    "lookups are then visible to your network. Off is strict mode: lookups fail "
                                    "instead."),
                    .visibleWhen = Condition{keys::DnsMode, {"custom", "doh"}}});
    defs.push_back({.key = keys::IpFamily,
                    .type = Type::Enum,
                    .defaultValue = str("auto"),
                    .options = {{"auto", QT_TRANSLATE_NOOP("Settings", "Auto")},
                                {"prefer_ipv4", QT_TRANSLATE_NOOP("Settings", "Prefer IPv4")},
                                {"prefer_ipv6", QT_TRANSLATE_NOOP("Settings", "Prefer IPv6")},
                                {"ipv4", QT_TRANSLATE_NOOP("Settings", "IPv4 only")},
                                {"ipv6", QT_TRANSLATE_NOOP("Settings", "IPv6 only")}},
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "IP version"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Auto tries IPv6 and IPv4 in parallel and keeps whichever connects first."),
                    .flags = flag::Advanced});
    defs.push_back({.key = keys::HostOverrides,
                    .type = Type::StringList,
                    .defaultValue = QVariant::fromValue(QStringList{}),
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "Host overrides"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Fixed addresses that skip DNS, as host=address. Repeat a host to give it "
                                    "several addresses."),
                    .flags = flag::Advanced,
                    .validator = validateHostOverrides});
    defs.push_back({.key = keys::DnsMinTtl,
                    .type = Type::Int,
                    .defaultValue = integer(60),
                    .min = 0,
                    .max = 3600,
                    .step = 1,
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "Minimum lifetime"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Answers are kept at least this long."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "s"),
                    .flags = flag::Advanced});
    defs.push_back({.key = keys::DnsMaxTtl,
                    .type = Type::Int,
                    .defaultValue = integer(3600),
                    .min = 60,
                    .max = 86400,
                    .step = 1,
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "Maximum lifetime"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Answers are looked up again after this long."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "s"),
                    .flags = flag::Advanced});
    defs.push_back(
        {.key = keys::DnsNegativeTtl,
         .type = Type::Int,
         .defaultValue = integer(30),
         .min = 0,
         .max = 3600,
         .step = 1,
         .category = Network,
         .label = QT_TRANSLATE_NOOP("Settings", "Failed lookups"),
         .description = QT_TRANSLATE_NOOP("Settings", "How long a name that does not exist is remembered as missing."),
         .unit = QT_TRANSLATE_NOOP("Settings", "s"),
         .flags = flag::Advanced});

    // --- Network: proxy ----------------------------------------------------------------------------
    defs.push_back({.key = keys::ProxyMode,
                    .type = Type::Enum,
                    .defaultValue = str("system"),
                    .options = {{"none", QT_TRANSLATE_NOOP("Settings", "No proxy")},
                                {"system", QT_TRANSLATE_NOOP("Settings", "System")},
                                {"manual", QT_TRANSLATE_NOOP("Settings", "Manual")}},
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "Proxy mode"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "System uses the proxy variables of the environment (http_proxy, no_proxy …) "
                                    "or, if none are set, your desktop's proxy settings."),
                    .cliAlias = "proxy-mode"});
    defs.push_back({.key = keys::ProxyType,
                    .type = Type::Enum,
                    .defaultValue = str("http"),
                    .options = {{"http", "HTTP", false},
                                {"https", "HTTPS", false},
                                {"socks4", "SOCKS4", false},
                                {"socks5", "SOCKS5", false},
                                {"socks5h", QT_TRANSLATE_NOOP("Settings", "SOCKS5 with remote DNS")}},
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "Proxy type"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "HTTPS encrypts the connection to the proxy itself. With SOCKS4 and SOCKS5 "
                                    "Noctara looks up server names itself; the others let the proxy do it."),
                    .visibleWhen = Condition{keys::ProxyMode, {"manual"}}});
    defs.push_back({.key = keys::ProxyHost,
                    .type = Type::String,
                    .defaultValue = str(""),
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "Proxy host"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Host name or IP address of the proxy server."),
                    .visibleWhen = Condition{keys::ProxyMode, {"manual"}},
                    .validator = validateHost});
    defs.push_back({.key = keys::ProxyPort,
                    .type = Type::Int,
                    .defaultValue = integer(8080),
                    .min = 1,
                    .max = 65535,
                    .step = 1,
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "Proxy port"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Username and password, if the proxy needs them, are kept in the system "
                                    "keyring, never in the configuration file."),
                    .visibleWhen = Condition{keys::ProxyMode, {"manual"}}});
    defs.push_back({.key = keys::ProxyBypass,
                    .type = Type::StringList,
                    .defaultValue = QVariant::fromValue(QStringList{}),
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "Connect directly to"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Hosts that bypass the proxy: example.com (with its subdomains), *.example.com, "
                                    "IP addresses or ranges such as 10.0.0.0/8."),
                    .visibleWhen = Condition{keys::ProxyMode, {"manual"}},
                    .validator = validateBypass});
    defs.push_back({.key = keys::ProxyBypassLocal,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "Bypass for local addresses"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Connect directly to localhost, private networks and names without a dot."),
                    .visibleWhen = Condition{keys::ProxyMode, {"manual"}}});

    // --- Network: connections ----------------------------------------------------------------------
    defs.push_back({.key = keys::HttpVersion,
                    .type = Type::Enum,
                    .defaultValue = str("auto"),
                    .options = {{"auto", QT_TRANSLATE_NOOP("Settings", "Auto")},
                                {"1.1", "HTTP/1.1", false},
                                {"2", "HTTP/2", false},
                                {"3", "HTTP/3", false}},
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "HTTP version"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Auto uses HTTP/2 where the server supports it. HTTP/3 falls back to older "
                                    "versions when it is blocked."),
                    .flags = flag::Advanced});
    defs.push_back({.key = keys::ConnectTimeout,
                    .type = Type::Int,
                    .defaultValue = integer(15),
                    .min = 1,
                    .max = 120,
                    .step = 1,
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "Connection timeout"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "How long to wait for a server, including the proxy and the encrypted "
                                    "handshake."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "s")});
    defs.push_back({.key = keys::ReadTimeout,
                    .type = Type::Int,
                    .defaultValue = integer(30),
                    .min = 5,
                    .max = 300,
                    .step = 5,
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "Stall timeout"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "A download is aborted (and retried) when no data arrives for this long."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "s")});
    defs.push_back({.key = keys::Retries,
                    .type = Type::Int,
                    .defaultValue = integer(2),
                    .min = 0,
                    .max = 10,
                    .step = 1,
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "Retries"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "How often a failed request is repeated. Only requests that are safe to repeat "
                                    "are retried.")});
    defs.push_back({.key = keys::RetryBackoff,
                    .type = Type::Int,
                    .defaultValue = integer(500),
                    .min = 100,
                    .max = 10000,
                    .step = 100,
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "Retry delay"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Wait before the first retry; it doubles with every further attempt."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "ms"),
                    .flags = flag::Advanced});
    defs.push_back({.key = keys::MaxConnections,
                    .type = Type::Int,
                    .defaultValue = integer(16),
                    .min = 1,
                    .max = 64,
                    .step = 1,
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "Maximum connections"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Open connections in total."),
                    .flags = flag::Advanced});
    defs.push_back({.key = keys::MaxConnectionsPerHost,
                    .type = Type::Int,
                    .defaultValue = integer(6),
                    .min = 1,
                    .max = 32,
                    .step = 1,
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "Connections per server"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Open connections to one server. HTTP/2 carries many requests over one."),
                    .flags = flag::Advanced});

    // --- Network: security -------------------------------------------------------------------------
    defs.push_back({.key = keys::TlsMinVersion,
                    .type = Type::Enum,
                    .defaultValue = str("1.2"),
                    .options = {{"1.2", "TLS 1.2", false}, {"1.3", "TLS 1.3", false}},
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "Minimum TLS version"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Older protocol versions are never used."),
                    .flags = flag::Advanced});
    defs.push_back({.key = keys::CaBundle,
                    .type = Type::Path,
                    .defaultValue = str(""),
                    .category = Network,
                    .label = QT_TRANSLATE_NOOP("Settings", "CA bundle"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "PEM file with the certificate authorities to trust instead of the system's, "
                                    "for networks that inspect encrypted traffic. Empty uses the system store."),
                    .flags = flag::Advanced});

    // --- Storage & Cache --------------------------------------------------------------------------
    defs.push_back({.key = keys::AudioCacheEnabled,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Storage,
                    .label = QT_TRANSLATE_NOOP("Settings", "Keep played tracks"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Off keeps only the current track in memory; nothing is written to disk."),
                    .cliAlias = "audio-cache"});
    defs.push_back({.key = keys::AudioDiskMb,
                    .type = Type::Int,
                    .defaultValue = integer(2048),
                    .min = 0,
                    .max = 102400,
                    .step = 256,
                    .category = Storage,
                    .label = QT_TRANSLATE_NOOP("Settings", "Disk space for tracks"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "The tracks played longest ago make room for new ones. 0 turns the disk cache "
                                    "off."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "MB"),
                    .visibleWhen = Condition{keys::AudioCacheEnabled, {"true"}}});
    defs.push_back({.key = keys::AudioRamMb,
                    .type = Type::Int,
                    .defaultValue = integer(128),
                    .min = 16,
                    .max = 2048,
                    .step = 16,
                    .category = Storage,
                    .label = QT_TRANSLATE_NOOP("Settings", "Memory for tracks"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "For the current and upcoming tracks, so seeking is instant. Released early when "
                                    "the system runs low on memory."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "MB")});
    defs.push_back({.key = keys::Prefetch,
                    .type = Type::Int,
                    .defaultValue = integer(2),
                    .min = 0,
                    .max = 5,
                    .step = 1,
                    .category = Storage,
                    .label = QT_TRANSLATE_NOOP("Settings", "Prepare upcoming tracks"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "How many tracks of the queue are downloaded in advance. None on a metered "
                                    "connection; at most one while the disk cache is off.")});
    defs.push_back({.key = keys::EmbedTags,
                    .type = Type::Bool,
                    .defaultValue = true,
                    .category = Storage,
                    .label = QT_TRANSLATE_NOOP("Settings", "Embed tags in cached audio"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Completed tracks get title, artist and cover inside the file. The audio itself "
                                    "is never altered."),
                    .flags = flag::Advanced,
                    .visibleWhen = Condition{keys::AudioCacheEnabled, {"true"}}});
    defs.push_back({.key = keys::CacheDir,
                    .type = Type::Path,
                    .defaultValue = str(""),
                    .category = Storage,
                    .label = QT_TRANSLATE_NOOP("Settings", "Cache location"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "A folder for all caches; Noctara creates a “noctara” folder inside it. Empty "
                                    "uses the standard cache folder of your system."),
                    .flags = flag::Advanced,
                    .validator = validateCacheDir});
    defs.push_back({.key = keys::ClearCacheOnExit,
                    .type = Type::Bool,
                    .defaultValue = false,
                    .category = Storage,
                    .label = QT_TRANSLATE_NOOP("Settings", "Clear cache on exit"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Removes cached tracks, images and metadata whenever Noctara quits.")});
    defs.push_back({.key = keys::MinFreeMb,
                    .type = Type::Int,
                    .defaultValue = integer(1024),
                    .min = 0,
                    .max = 102400,
                    .step = 256,
                    .category = Storage,
                    .label = QT_TRANSLATE_NOOP("Settings", "Keep free on disk"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "The cache gives space back before the disk gets fuller than this."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "MB"),
                    .flags = flag::Advanced});
    defs.push_back({.key = keys::ImageRamMb,
                    .type = Type::Int,
                    .defaultValue = integer(64),
                    .min = 16,
                    .max = 1024,
                    .step = 16,
                    .category = Storage,
                    .label = QT_TRANSLATE_NOOP("Settings", "Memory for images"),
                    .description = QT_TRANSLATE_NOOP("Settings", "Decoded images ready to be shown."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "MB"),
                    .flags = flag::Advanced});
    defs.push_back({.key = keys::ImageDiskMb,
                    .type = Type::Int,
                    .defaultValue = integer(512),
                    .min = 0,
                    .max = 10240,
                    .step = 64,
                    .category = Storage,
                    .label = QT_TRANSLATE_NOOP("Settings", "Disk space for images"),
                    .description = QT_TRANSLATE_NOOP("Settings", "0 keeps images in memory only."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "MB")});
    defs.push_back({.key = keys::MetadataDiskMb,
                    .type = Type::Int,
                    .defaultValue = integer(128),
                    .min = 0,
                    .max = 2048,
                    .step = 16,
                    .category = Storage,
                    .label = QT_TRANSLATE_NOOP("Settings", "Disk space for metadata"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "0 turns the metadata cache off; pages then always load from the network."),
                    .unit = QT_TRANSLATE_NOOP("Settings", "MB"),
                    .flags = flag::Advanced});

    // --- Library -------------------------------------------------------------------------------
    defs.push_back({.key = keys::PageSize,
                    .type = Type::Int,
                    .defaultValue = integer(50),
                    .min = 10,
                    .max = 200,
                    .step = 10,
                    .category = Library,
                    .label = QT_TRANSLATE_NOOP("Settings", "Items per request"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Lists load more as you scroll. Larger requests mean fewer pauses but more data "
                                    "up front."),
                    .flags = flag::Advanced});

    // --- Advanced ------------------------------------------------------------------------------
    defs.push_back({.key = keys::ClientId,
                    .type = Type::String,
                    .defaultValue = str(""),
                    .category = Advanced,
                    .label = QT_TRANSLATE_NOOP("Settings", "Client ID"),
                    .description = QT_TRANSLATE_NOOP(
                        "Settings", "Empty: found automatically in SoundCloud's web player. Set it only if that "
                                    "stops working; it is not a secret and not tied to your account."),
                    .validator = validateClientId});

    return defs;
}

}  // namespace

const Schema& Schema::builtin()
{
    static const Schema schema(builtinSections(), builtinDefinitions());
    return schema;
}

}  // namespace noctara::settings
