#include "settings/Schema.hpp"

#include "core/Strings.hpp"

#include <QLocale>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <limits>
#include <print>

namespace noctara::settings {

namespace {

[[noreturn]] void schemaDefect(const std::string& message)
{
    std::println(stderr, "settings schema defect: {}", message);
    std::abort();
}

std::string keyString(const Definition& def)
{
    return std::string(def.key);
}

Result<QVariant> validateNumber(const Definition& def, const QVariant& candidate)
{
    double number = 0.0;
    switch (candidate.typeId()) {
    case QMetaType::LongLong:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::ULongLong: number = static_cast<double>(candidate.toLongLong()); break;
    case QMetaType::Double:
    case QMetaType::Float: number = candidate.toDouble(); break;
    default: return fail(ErrorCode::ConfigTypeMismatch, std::format("{}: expected a number", def.key));
    }
    if (!std::isfinite(number)) {
        return fail(ErrorCode::ConfigInvalidValue, std::format("{}: value must be finite", def.key));
    }
    if (def.type == Type::Int && std::trunc(number) != number) {
        return fail(ErrorCode::ConfigTypeMismatch, std::format("{}: expected an integer", def.key));
    }
    if (def.bounded() && (number < def.min || number > def.max)) {
        return fail(ErrorCode::ConfigOutOfRange,
                    std::format("{}: {} is outside the range {} .. {}", def.key, number, def.min, def.max));
    }
    if (def.type == Type::Int) {
        return QVariant::fromValue(static_cast<qint64>(number));
    }
    return QVariant::fromValue(number);
}

Result<QVariant> validateColor(const Definition& def, const QString& text)
{
    static const QRegularExpression pattern(QStringLiteral("^#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})$"));
    if (!pattern.match(text).hasMatch()) {
        return fail(ErrorCode::ConfigInvalidValue,
                    std::format("{}: '{}' is not a #rrggbb or #aarrggbb colour", def.key, text.toStdString()));
    }
    return QVariant::fromValue(text.toLower());
}

Result<QVariant> validateShape(const Definition& def, const QVariant& candidate)
{
    switch (def.type) {
    case Type::Bool:
        if (candidate.typeId() != QMetaType::Bool) {
            return fail(ErrorCode::ConfigTypeMismatch, std::format("{}: expected true or false", def.key));
        }
        return candidate;
    case Type::Int:
    case Type::Real: return validateNumber(def, candidate);
    case Type::StringList: {
        if (candidate.typeId() != QMetaType::QStringList) {
            return fail(ErrorCode::ConfigTypeMismatch, std::format("{}: expected a list of strings", def.key));
        }
        return candidate;
    }
    case Type::String:
    case Type::Enum:
    case Type::Color:
    case Type::Path:
    case Type::Url:
    case Type::Shortcut: break;
    }

    if (candidate.typeId() != QMetaType::QString) {
        return fail(ErrorCode::ConfigTypeMismatch, std::format("{}: expected a string", def.key));
    }
    const QString text = candidate.toString();
    if (def.type == Type::Enum) {
        const auto it =
            std::ranges::find_if(def.options, [&](const Option& option) { return latin(option.id) == text; });
        if (it == def.options.end()) {
            return fail(ErrorCode::ConfigUnknownEnumValue,
                        std::format("{}: '{}' is not one of the allowed values", def.key, text.toStdString()));
        }
        return candidate;
    }
    if (def.type == Type::Color) {
        return validateColor(def, text);
    }
    if (text.contains(QChar(u'\0'))) {
        return fail(ErrorCode::ConfigInvalidValue, std::format("{}: embedded NUL character", def.key));
    }
    return candidate;
}

}  // namespace

std::string_view categoryId(Category category) noexcept
{
    switch (category) {
    case Category::General: return "general";
    case Category::Appearance: return "appearance";
    case Category::Layout: return "layout";
    case Category::Playback: return "playback";
    case Category::Library: return "library";
    case Category::Keyboard: return "keyboard";
    case Category::Performance: return "performance";
    case Category::Network: return "network";
    case Category::Storage: return "storage";
    case Category::Accounts: return "accounts";
    case Category::Advanced: return "advanced";
    case Category::About: return "about";
    }
    return "general";
}

const char* categoryLabel(Category category) noexcept
{
    switch (category) {
    case Category::General: return QT_TRANSLATE_NOOP("Settings", "General");
    case Category::Appearance: return QT_TRANSLATE_NOOP("Settings", "Appearance");
    case Category::Layout: return QT_TRANSLATE_NOOP("Settings", "Layout");
    case Category::Playback: return QT_TRANSLATE_NOOP("Settings", "Playback");
    case Category::Library: return QT_TRANSLATE_NOOP("Settings", "Library & Content");
    case Category::Keyboard: return QT_TRANSLATE_NOOP("Settings", "Keyboard");
    case Category::Performance: return QT_TRANSLATE_NOOP("Settings", "Performance");
    case Category::Network: return QT_TRANSLATE_NOOP("Settings", "Network");
    case Category::Storage: return QT_TRANSLATE_NOOP("Settings", "Storage & Cache");
    case Category::Accounts: return QT_TRANSLATE_NOOP("Settings", "Accounts");
    case Category::Advanced: return QT_TRANSLATE_NOOP("Settings", "Advanced");
    case Category::About: return QT_TRANSLATE_NOOP("Settings", "About");
    }
    return "";
}

std::string_view Definition::sectionId() const noexcept
{
    const auto dot = key.rfind('.');
    return dot == std::string_view::npos ? std::string_view{} : key.substr(0, dot);
}

std::string_view Definition::leafName() const noexcept
{
    const auto dot = key.rfind('.');
    return dot == std::string_view::npos ? key : key.substr(dot + 1);
}

Schema::Schema(std::vector<Section> sections, std::vector<Definition> definitions)
    : m_sections(std::move(sections))
    , m_definitions(std::move(definitions))
{
    static const QRegularExpression keyPattern(QStringLiteral("^[a-z][a-z0-9_]*(\\.[a-z][a-z0-9_]*)+$"));
    m_index.reserve(m_definitions.size());
    for (std::size_t i = 0; i < m_definitions.size(); ++i) {
        const Definition& def = m_definitions[i];
        if (!keyPattern.match(latin(def.key)).hasMatch()) {
            schemaDefect(std::format("invalid key '{}'", def.key));
        }
        if (!m_index.emplace(def.key, i).second) {
            schemaDefect(std::format("duplicate key '{}'", def.key));
        }
        const Section* sec = section(def.sectionId());
        if (sec == nullptr) {
            schemaDefect(std::format("key '{}' has no declared section '{}'", def.key, def.sectionId()));
        }
        if (sec->category != def.category) {
            schemaDefect(std::format("key '{}' category differs from its section", def.key));
        }
        // Shortcut rows are explained once by their section (the label says what the keys do).
        if (def.label == nullptr || (def.description == nullptr && def.type != Type::Shortcut)) {
            schemaDefect(std::format("key '{}' lacks a label or description", def.key));
        }
        if (def.type == Type::Enum && def.options.empty()) {
            schemaDefect(std::format("enum key '{}' has no options", def.key));
        }
        auto checked = validate(def, def.defaultValue);
        if (!checked) {
            schemaDefect(std::format("default of '{}' is invalid: {}", def.key, checked.error().describe()));
        }
        m_definitions[i].defaultValue = *checked;
    }
    for (const Definition& def : m_definitions) {
        if (def.visibleWhen && !m_index.contains(def.visibleWhen->key)) {
            schemaDefect(std::format("key '{}' depends on unknown key '{}'", keyString(def), def.visibleWhen->key));
        }
        // Visibility follows the chain of dependencies, so it must end.
        std::size_t depth = 0;
        for (const Definition* parent = &def; parent != nullptr && parent->visibleWhen;
             parent = find(parent->visibleWhen->key)) {
            if (++depth > m_definitions.size()) {
                schemaDefect(std::format("key '{}' has a cyclic visibility dependency", keyString(def)));
            }
        }
    }
}

std::optional<std::size_t> Schema::indexOf(std::string_view key) const noexcept
{
    const auto it = m_index.find(key);
    if (it == m_index.end()) {
        return std::nullopt;
    }
    return it->second;
}

const Definition* Schema::find(std::string_view key) const noexcept
{
    const auto index = indexOf(key);
    return index ? &m_definitions[*index] : nullptr;
}

const Section* Schema::section(std::string_view id) const noexcept
{
    const auto it = std::ranges::find(m_sections, id, &Section::id);
    return it == m_sections.end() ? nullptr : &*it;
}

QString Schema::environmentVariable(const Definition& def)
{
    QString name = QStringLiteral("NOCTARA_") + latin(def.key).toUpper();
    name.replace(QChar(u'.'), QChar(u'_'));
    return name;
}

Result<QVariant> validate(const Definition& def, const QVariant& candidate)
{
    auto shaped = validateShape(def, candidate);
    if (!shaped) {
        return shaped;
    }
    if (def.validator != nullptr) {
        if (auto custom = def.validator(*shaped); !custom) {
            Error error = std::move(custom.error());
            error.detail = std::format("{}: {}", def.key, error.detail);
            return std::unexpected(std::move(error));
        }
    }
    return shaped;
}

Result<QVariant> parseText(const Definition& def, QStringView text)
{
    const QString trimmed = text.trimmed().toString();
    switch (def.type) {
    case Type::Bool: {
        const QString lower = trimmed.toLower();
        if (lower == u"true" || lower == u"1" || lower == u"on" || lower == u"yes") {
            return validate(def, true);
        }
        if (lower == u"false" || lower == u"0" || lower == u"off" || lower == u"no") {
            return validate(def, false);
        }
        return fail(ErrorCode::ConfigTypeMismatch, std::format("{}: expected true or false", def.key));
    }
    case Type::Int: {
        bool ok = false;
        const qint64 number = trimmed.toLongLong(&ok);
        if (!ok) {
            return fail(ErrorCode::ConfigTypeMismatch, std::format("{}: expected an integer", def.key));
        }
        return validate(def, QVariant::fromValue(number));
    }
    case Type::Real: {
        bool ok = false;
        const double number = trimmed.toDouble(&ok);
        if (!ok) {
            return fail(ErrorCode::ConfigTypeMismatch, std::format("{}: expected a number", def.key));
        }
        return validate(def, QVariant::fromValue(number));
    }
    case Type::StringList: {
        QStringList items;
        for (const QStringView item : QStringView(trimmed).split(u',', Qt::SkipEmptyParts)) {
            items.append(item.trimmed().toString());
        }
        return validate(def, QVariant::fromValue(items));
    }
    case Type::String:
    case Type::Enum:
    case Type::Color:
    case Type::Path:
    case Type::Url:
    case Type::Shortcut: return validate(def, QVariant::fromValue(trimmed));
    }
    return fail(ErrorCode::ConfigTypeMismatch, std::string(def.key));
}

namespace {

QString tomlString(const QString& text)
{
    QString out;
    out.reserve(text.size() + 2);
    out += u'"';
    for (const QChar ch : text) {
        switch (ch.unicode()) {
        case u'"': out += QStringLiteral("\\\""); break;
        case u'\\': out += QStringLiteral("\\\\"); break;
        case u'\n': out += QStringLiteral("\\n"); break;
        case u'\r': out += QStringLiteral("\\r"); break;
        case u'\t': out += QStringLiteral("\\t"); break;
        default:
            if (ch.unicode() < 0x20 || ch.unicode() == 0x7f) {
                out += QStringLiteral("\\u%1").arg(static_cast<uint>(ch.unicode()), 4, 16, QChar(u'0'));
            } else {
                out += ch;
            }
        }
    }
    out += u'"';
    return out;
}

}  // namespace

QString formatTomlValue(const Definition& def, const QVariant& value)
{
    switch (def.type) {
    case Type::Bool: return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case Type::Int: return QString::number(value.toLongLong());
    case Type::Real: {
        QString text = QString::number(value.toDouble(), 'g', QLocale::FloatingPointShortest);
        if (!text.contains(u'.') && !text.contains(u'e') && !text.contains(u"inf") && !text.contains(u"nan")) {
            text += QStringLiteral(".0");
        }
        return text;
    }
    case Type::StringList: {
        QStringList parts;
        for (const QString& item : value.toStringList()) {
            parts.append(tomlString(item));
        }
        return u'[' + parts.join(QStringLiteral(", ")) + u']';
    }
    case Type::String:
    case Type::Enum:
    case Type::Color:
    case Type::Path:
    case Type::Url:
    case Type::Shortcut: return tomlString(value.toString());
    }
    return {};
}

std::string_view typeName(Type type) noexcept
{
    switch (type) {
    case Type::Bool: return "bool";
    case Type::Int: return "integer";
    case Type::Real: return "number";
    case Type::String: return "string";
    case Type::Enum: return "enum";
    case Type::Color: return "color";
    case Type::Path: return "path";
    case Type::Url: return "url";
    case Type::StringList: return "string list";
    case Type::Shortcut: return "shortcut";
    }
    return "?";
}

}  // namespace noctara::settings
