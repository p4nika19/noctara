#include "theme/UserTheme.hpp"

#include "settings/TomlInclude.hpp"

#include <QColor>

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace noctara::theme {

namespace {

template <class Tokens, class Value> struct Field {
    std::string_view name;
    Value Tokens::* member;
    double max = 0.0;  // 0: the table's limit
};

constexpr std::array kRadius{
    Field<RadiusTokens, qreal>{"xs", &RadiusTokens::xs},         Field<RadiusTokens, qreal>{"sm", &RadiusTokens::sm},
    Field<RadiusTokens, qreal>{"md", &RadiusTokens::md},         Field<RadiusTokens, qreal>{"lg", &RadiusTokens::lg},
    Field<RadiusTokens, qreal>{"button", &RadiusTokens::button},
};

constexpr double kMaxRailWidth = 400.0;  // the expanded rail is 220 px by default

constexpr std::array kSpace{
    Field<SpacingTokens, qreal>{"xxs", &SpacingTokens::xxs},
    Field<SpacingTokens, qreal>{"xs", &SpacingTokens::xs},
    Field<SpacingTokens, qreal>{"sm", &SpacingTokens::sm},
    Field<SpacingTokens, qreal>{"md", &SpacingTokens::md},
    Field<SpacingTokens, qreal>{"lg", &SpacingTokens::lg},
    Field<SpacingTokens, qreal>{"xl", &SpacingTokens::xl},
    Field<SpacingTokens, qreal>{"xxl", &SpacingTokens::xxl},
    Field<SpacingTokens, qreal>{"xxxl", &SpacingTokens::xxxl},
    Field<SpacingTokens, qreal>{"controlHeight", &SpacingTokens::controlHeight},
    Field<SpacingTokens, qreal>{"controlHeightSmall", &SpacingTokens::controlHeightSmall},
    Field<SpacingTokens, qreal>{"iconSize", &SpacingTokens::iconSize},
    Field<SpacingTokens, qreal>{"iconSizeSmall", &SpacingTokens::iconSizeSmall},
    Field<SpacingTokens, qreal>{"iconSizeLarge", &SpacingTokens::iconSizeLarge},
    Field<SpacingTokens, qreal>{"navRailWidth", &SpacingTokens::navRailWidth, kMaxRailWidth},
    Field<SpacingTokens, qreal>{"navRailWidthExpanded", &SpacingTokens::navRailWidthExpanded, kMaxRailWidth},
    Field<SpacingTokens, qreal>{"nowPlayingHeight", &SpacingTokens::nowPlayingHeight},
    Field<SpacingTokens, qreal>{"rowHeight", &SpacingTokens::rowHeight},
    Field<SpacingTokens, qreal>{"pageMargin", &SpacingTokens::pageMargin},
};

constexpr std::array kMotion{
    Field<MotionTokens, int>{"fast", &MotionTokens::fast},
    Field<MotionTokens, int>{"normal", &MotionTokens::normal},
    Field<MotionTokens, int>{"slow", &MotionTokens::slow},
    Field<MotionTokens, int>{"theme", &MotionTokens::theme},
    Field<MotionTokens, int>{"backdrop", &MotionTokens::backdrop},
    Field<MotionTokens, int>{"shimmer", &MotionTokens::shimmer},
};

struct GlassField {
    std::string_view name;
    qreal GlassTokens::* member;
    double min;
    double max;
};

// Tint and grain are settings (sliders), so a theme leaves them alone.
constexpr std::array kGlass{
    GlassField{"rimWidth", &GlassTokens::rimWidth, 0.0, 4.0},
    GlassField{"refraction", &GlassTokens::refraction, 0.0, 64.0},
    GlassField{"thickness", &GlassTokens::thickness, 0.0, 64.0},
    GlassField{"dispersion", &GlassTokens::dispersion, 0.0, 1.0},
    GlassField{"specular", &GlassTokens::specular, 0.0, 1.0},
    GlassField{"fresnel", &GlassTokens::fresnel, 0.0, 1.0},
    GlassField{"rimDarkening", &GlassTokens::rimDarkening, 0.0, 1.0},
    GlassField{"saturationLift", &GlassTokens::saturationLift, 0.0, 1.0},
    GlassField{"brightnessLift", &GlassTokens::brightnessLift, -0.5, 0.5},
    GlassField{"lightAngle", &GlassTokens::lightAngle, 0.0, 360.0},
};

constexpr double kMaxRadius = 64.0;
constexpr double kMaxSpace = 160.0;
constexpr int kMaxDurationMs = 5000;

struct PaletteRole {
    std::string_view name;
    color::Rgba Palette::* member;
};

const std::vector<PaletteRole>& paletteRoles()
{
    static const std::vector<PaletteRole> roles{
#define NOCTARA_ROLE_ENTRY(role) PaletteRole{#role, &Palette::role},
        NOCTARA_PALETTE_ROLES(NOCTARA_ROLE_ENTRY)
#undef NOCTARA_ROLE_ENTRY
    };
    return roles;
}

QString qt(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

int lineOf(const toml::node& node)
{
    return static_cast<int>(node.source().begin.line);
}

// "#rrggbb" or "#aarrggbb" (the same notation as colour settings).
std::optional<color::Rgba> parseColor(std::string_view text)
{
    if ((text.size() != 7 && text.size() != 9) || text.front() != '#') {
        return std::nullopt;
    }
    const QColor parsed = QColor::fromString(qt(text));
    if (!parsed.isValid()) {
        return std::nullopt;
    }
    return color::Rgba{
        {static_cast<double>(parsed.redF()), static_cast<double>(parsed.greenF()), static_cast<double>(parsed.blueF())},
        static_cast<double>(parsed.alphaF())};
}

class Reader {
public:
    explicit Reader(std::vector<ThemeIssue>& issues)
        : m_issues(issues)
    {
    }

    void unknown(const QString& key, const toml::node& node) const
    {
        m_issues.push_back({ThemeIssue::Kind::UnknownToken, key, QStringLiteral("not a theme token"), lineOf(node)});
    }
    void invalid(const QString& key, const toml::node& node, const char* expected) const
    {
        m_issues.push_back({ThemeIssue::Kind::InvalidValue, key, QString::fromLatin1(expected), lineOf(node)});
    }

    std::optional<double> number(const QString& key, const toml::node& node, double min, double max) const
    {
        std::optional<double> value;
        if (const auto* real = node.as_floating_point()) {
            value = real->get();
        } else if (const auto* integer = node.as_integer()) {
            value = static_cast<double>(integer->get());
        }
        if (!value || !std::isfinite(*value) || *value < min || *value > max) {
            m_issues.push_back({ThemeIssue::Kind::InvalidValue, key,
                                QString::fromStdString(std::format("a number from {} to {}", min, max)), lineOf(node)});
            return std::nullopt;
        }
        return value;
    }

    std::optional<QString> text(const QString& key, const toml::node& node) const
    {
        if (const auto* string = node.as_string()) {
            return qt(string->get());
        }
        invalid(key, node, "a string");
        return std::nullopt;
    }

    std::optional<color::Rgba> colour(const QString& key, const toml::node& node) const
    {
        if (const auto* string = node.as_string()) {
            if (auto parsed = parseColor(string->get())) {
                return parsed;
            }
        }
        invalid(key, node, "a #rrggbb or #aarrggbb colour");
        return std::nullopt;
    }

    const toml::table* table(const QString& key, const toml::node& node) const
    {
        if (const auto* table = node.as_table()) {
            return table;
        }
        invalid(key, node, "a table");
        return nullptr;
    }

private:
    std::vector<ThemeIssue>& m_issues;
};

std::optional<int> schemeIndex(std::string_view name)
{
    if (name == "dark") {
        return static_cast<int>(Scheme::Dark);
    }
    if (name == "light") {
        return static_cast<int>(Scheme::Light);
    }
    return std::nullopt;
}

template <class Fields> bool known(const Fields& fields, std::string_view name)
{
    return std::ranges::any_of(fields, [name](const auto& field) { return field.name == name; });
}

void readMeta(const Reader& reader, const toml::table& table, UserTheme& theme)
{
    for (const auto& [rawKey, node] : table) {
        const std::string_view name = rawKey.str();
        const QString key = QStringLiteral("theme.") + qt(name);
        if (name == "name" || name == "description" || name == "author") {
            if (const auto value = reader.text(key, node)) {
                (name == "name"          ? theme.name
                 : name == "description" ? theme.description
                                         : theme.author) = value->trimmed();
            }
        } else if (name == "accent") {
            if (const auto value = reader.colour(key, node)) {
                theme.accent = color::toOkLch(value->rgb);
            }
        } else {
            reader.unknown(key, node);
        }
    }
}

void readColors(const Reader& reader, const toml::table& table, UserTheme& theme)
{
    for (const auto& [schemeKey, schemeNode] : table) {
        const QString prefix = QStringLiteral("colors.") + qt(schemeKey.str());
        const std::optional<int> scheme = schemeIndex(schemeKey.str());
        if (!scheme) {
            reader.unknown(prefix, schemeNode);
            continue;
        }
        const toml::table* roles = reader.table(prefix, schemeNode);
        if (roles == nullptr) {
            continue;
        }
        for (const auto& [roleKey, node] : *roles) {
            const QString key = prefix + u'.' + qt(roleKey.str());
            if (!known(paletteRoles(), roleKey.str())) {
                reader.unknown(key, node);
            } else if (const auto value = reader.colour(key, node)) {
                theme.colors[static_cast<std::size_t>(*scheme)][std::string(roleKey.str())] = *value;
            }
        }
    }
}

template <class Fields>
void readNumbers(const Reader& reader, const toml::table& table, const QString& prefix, const Fields& fields,
                 double max, std::map<std::string, double>& out)
{
    for (const auto& [rawKey, node] : table) {
        const QString key = prefix + u'.' + qt(rawKey.str());
        const auto field = std::ranges::find(fields, rawKey.str(), &Fields::value_type::name);
        if (field == std::ranges::end(fields)) {
            reader.unknown(key, node);
        } else if (const auto value = reader.number(key, node, 0.0, field->max > 0.0 ? field->max : max)) {
            out[std::string(rawKey.str())] = *value;
        }
    }
}

void readFonts(const Reader& reader, const toml::table& table, UserTheme& theme)
{
    for (const auto& [rawKey, node] : table) {
        const std::string_view name = rawKey.str();
        const QString key = QStringLiteral("fonts.") + qt(name);
        QString* target = name == "heading" ? &theme.headingFont
                          : name == "body"  ? &theme.bodyFont
                          : name == "mono"  ? &theme.monoFont
                                            : nullptr;
        if (target == nullptr) {
            reader.unknown(key, node);
        } else if (const auto value = reader.text(key, node)) {
            *target = value->trimmed();
        }
    }
}

void readMotion(const Reader& reader, const toml::table& table, UserTheme& theme)
{
    for (const auto& [rawKey, node] : table) {
        const QString key = QStringLiteral("motion.") + qt(rawKey.str());
        if (!known(kMotion, rawKey.str())) {
            reader.unknown(key, node);
        } else if (const auto value = reader.number(key, node, 0.0, kMaxDurationMs)) {
            theme.motion[std::string(rawKey.str())] = static_cast<int>(std::lround(*value));
        }
    }
}

void readGlass(const Reader& reader, const toml::table& table, UserTheme& theme)
{
    for (const auto& [schemeKey, schemeNode] : table) {
        const QString prefix = QStringLiteral("glass.") + qt(schemeKey.str());
        const std::optional<int> scheme = schemeIndex(schemeKey.str());
        if (!scheme) {
            reader.unknown(prefix, schemeNode);
            continue;
        }
        const toml::table* values = reader.table(prefix, schemeNode);
        if (values == nullptr) {
            continue;
        }
        for (const auto& [rawKey, node] : *values) {
            const QString key = prefix + u'.' + qt(rawKey.str());
            const auto field = std::ranges::find(kGlass, rawKey.str(), &GlassField::name);
            if (field == kGlass.end()) {
                reader.unknown(key, node);
            } else if (const auto value = reader.number(key, node, field->min, field->max)) {
                theme.glass[static_cast<std::size_t>(*scheme)][std::string(rawKey.str())] = *value;
            }
        }
    }
}

}  // namespace

Result<UserTheme> parseUserTheme(QByteArrayView toml, const QString& id, std::vector<ThemeIssue>& issues)
{
    toml::parse_result result =
        toml::parse(std::string_view(toml.data(), static_cast<std::size_t>(toml.size())), id.toStdString());
    if (!result) {
        const toml::parse_error& error = result.error();
        issues.push_back(
            {ThemeIssue::Kind::Syntax, {}, qt(error.description()), static_cast<int>(error.source().begin.line)});
        return fail(ErrorCode::ConfigSyntaxError, std::string(error.description()));
    }
    const toml::table root = std::move(result).table();
    const Reader reader(issues);
    UserTheme theme;
    theme.id = id;
    for (const auto& [rawKey, node] : root) {
        const std::string_view name = rawKey.str();
        const QString key = qt(name);
        const toml::table* table = node.as_table();
        if (table == nullptr) {
            // Top-level values are not tokens (everything lives in tables).
            if (name == "name" || name == "description" || name == "author" || name == "accent" || name == "colors" ||
                name == "radius" || name == "space" || name == "fonts" || name == "motion" || name == "glass" ||
                name == "theme") {
                reader.invalid(key, node, "a table");
            } else {
                reader.unknown(key, node);
            }
            continue;
        }
        if (name == "theme") {
            readMeta(reader, *table, theme);
        } else if (name == "colors") {
            readColors(reader, *table, theme);
        } else if (name == "radius") {
            readNumbers(reader, *table, key, kRadius, kMaxRadius, theme.radius);
        } else if (name == "space") {
            readNumbers(reader, *table, key, kSpace, kMaxSpace, theme.space);
        } else if (name == "fonts") {
            readFonts(reader, *table, theme);
        } else if (name == "motion") {
            readMotion(reader, *table, theme);
        } else if (name == "glass") {
            readGlass(reader, *table, theme);
        } else {
            reader.unknown(key, node);
        }
    }
    if (theme.name.isEmpty()) {
        theme.name = id;
    }
    return theme;
}

void applyColors(const UserTheme& theme, Scheme scheme, Palette& palette)
{
    for (const auto& [name, value] : theme.colors[static_cast<std::size_t>(scheme)]) {
        const auto role = std::ranges::find(paletteRoles(), std::string_view(name), &PaletteRole::name);
        if (role != paletteRoles().end()) {
            palette.*role->member = value;
        }
    }
}

void applyRadius(const UserTheme& theme, RadiusTokens& radius)
{
    for (const auto& field : kRadius) {
        if (const auto it = theme.radius.find(std::string(field.name)); it != theme.radius.end()) {
            radius.*field.member = it->second;
        }
    }
}

void applySpace(const UserTheme& theme, SpacingTokens& space)
{
    for (const auto& field : kSpace) {
        if (const auto it = theme.space.find(std::string(field.name)); it != theme.space.end()) {
            space.*field.member = it->second;
        }
    }
}

void applyMotion(const UserTheme& theme, MotionTokens& motion)
{
    for (const auto& field : kMotion) {
        if (const auto it = theme.motion.find(std::string(field.name)); it != theme.motion.end()) {
            motion.*field.member = it->second;
        }
    }
}

void applyGlass(const UserTheme& theme, Scheme scheme, GlassTokens& glass)
{
    const auto& values = theme.glass[static_cast<std::size_t>(scheme)];
    for (const auto& field : kGlass) {
        if (const auto it = values.find(std::string(field.name)); it != values.end()) {
            glass.*field.member = it->second;
        }
    }
}

}  // namespace noctara::theme
