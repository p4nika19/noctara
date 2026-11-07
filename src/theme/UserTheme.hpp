#pragma once

#include "core/Error.hpp"
#include "theme/ColorScience.hpp"
#include "theme/Palette.hpp"
#include "theme/Tokens.hpp"

#include <QByteArrayView>
#include <QString>

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace noctara::theme {

// A problem in a theme file. Files are data only: unknown tokens are reported and skipped, invalid values are
// ignored (the built-in value stays), and only a syntax error rejects the whole file.
struct ThemeIssue {
    enum class Kind : std::uint8_t {
        Syntax,        // the file is not valid TOML: nothing of it applies
        UnknownToken,  // a table or key the token schema does not know
        InvalidValue,  // wrong type or out of range; ignored
    };
    Kind kind = Kind::UnknownToken;
    QString key;     // dotted path, e.g. "colors.dark.accent"
    QString detail;  // English, for logs (expected format or parser message)
    int line = 0;

    friend bool operator==(const ThemeIssue&, const ThemeIssue&) = default;
};

// A user theme (<config>/themes/<id>.toml): overrides for any subset of the design tokens.
struct UserTheme {
    QString id;  // file name without .toml
    QString name;
    QString description;
    QString author;
    std::optional<color::OkLch> accent;                        // seed when the cover art does not decide the accent
    std::array<std::map<std::string, color::Rgba>, 2> colors;  // [Scheme::Dark], [Scheme::Light]
    std::map<std::string, double> radius;
    std::map<std::string, double> space;
    QString headingFont;
    QString bodyFont;
    QString monoFont;
    std::map<std::string, int> motion;
    std::array<std::map<std::string, double>, 2> glass;
};

// Parses and validates a theme file. Fails only on a TOML syntax error (ErrorCode::ConfigSyntaxError); every
// other problem is reported in `issues` and leaves that token at its built-in value.
[[nodiscard]] Result<UserTheme> parseUserTheme(QByteArrayView toml, const QString& id, std::vector<ThemeIssue>& issues);

// Apply a theme's overrides (sizes before the UI scale, durations before the animation speed).
void applyColors(const UserTheme& theme, Scheme scheme, Palette& palette);
void applyRadius(const UserTheme& theme, RadiusTokens& radius);
void applySpace(const UserTheme& theme, SpacingTokens& space);
void applyMotion(const UserTheme& theme, MotionTokens& motion);
void applyGlass(const UserTheme& theme, Scheme scheme, GlassTokens& glass);

}  // namespace noctara::theme
