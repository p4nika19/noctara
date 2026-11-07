#include "settings/TomlCodec.hpp"

#include "core/Strings.hpp"
#include "settings/Migrations.hpp"

#include <QStringList>

#include <format>
#include <sstream>

namespace noctara::settings {

namespace {

constexpr std::string_view kVersionKey = "config_version";

QString fromUtf8(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

toml::table* tableAt(toml::table& root, std::string_view path)
{
    toml::table* current = &root;
    if (path.empty()) {
        return current;
    }
    for (const auto part : splitPath(path)) {
        current = current->get_as<toml::table>(part);
        if (current == nullptr) {
            return nullptr;
        }
    }
    return current;
}

ConfigIssue issueAt(const toml::node& node, const QString& file, ConfigIssue::Severity severity, ErrorCode code,
                    QString key, QString detail)
{
    const auto& begin = node.source().begin;
    return ConfigIssue{
        severity,         code, file, static_cast<int>(begin.line), static_cast<int>(begin.column), std::move(key),
        std::move(detail)};
}

// Converts a TOML node into the QVariant shape expected by validate(), or an error message.
Result<QVariant> toVariant(const Definition& def, const toml::node& node)
{
    switch (def.type) {
    case Type::Bool:
        if (const auto* value = node.as_boolean()) {
            return QVariant::fromValue(value->get());
        }
        break;
    case Type::Int:
    case Type::Real:
        if (const auto* value = node.as_integer()) {
            return QVariant::fromValue(static_cast<qint64>(value->get()));
        }
        if (const auto* value = node.as_floating_point()) {
            return QVariant::fromValue(value->get());
        }
        break;
    case Type::StringList:
        if (const auto* array = node.as_array()) {
            QStringList items;
            items.reserve(static_cast<qsizetype>(array->size()));
            for (const toml::node& element : *array) {
                const auto* text = element.as_string();
                if (text == nullptr) {
                    return fail(ErrorCode::ConfigTypeMismatch, std::format("{}: list items must be strings", def.key));
                }
                items.append(fromUtf8(text->get()));
            }
            return QVariant::fromValue(items);
        }
        break;
    case Type::String:
    case Type::Enum:
    case Type::Color:
    case Type::Path:
    case Type::Url:
    case Type::Shortcut:
        if (const auto* value = node.as_string()) {
            return QVariant::fromValue(fromUtf8(value->get()));
        }
        break;
    }
    return fail(ErrorCode::ConfigTypeMismatch, std::format("{}: expected {}", def.key, typeName(def.type)));
}

// Removes empty tables bottom-up so only genuinely unknown content survives.
bool pruneEmpty(toml::table& table)
{
    std::vector<std::string> emptyChildren;
    for (auto&& [key, node] : table) {
        if (auto* child = node.as_table(); child != nullptr && pruneEmpty(*child)) {
            emptyChildren.emplace_back(key.str());
        }
    }
    for (const auto& key : emptyChildren) {
        table.erase(key);
    }
    return table.empty();
}

void collectUnknownLeaves(const toml::table& table, const std::string& prefix, const QString& file,
                          ConfigIssues& issues)
{
    for (auto&& [key, node] : table) {
        const std::string path = prefix.empty() ? std::string(key.str()) : prefix + "." + std::string(key.str());
        if (const auto* child = node.as_table(); child != nullptr && !child->is_inline()) {
            collectUnknownLeaves(*child, path, file, issues);
        } else {
            issues.append(issueAt(node, file, ConfigIssue::Severity::Warning, ErrorCode::ConfigUnknownKey,
                                  fromUtf8(path), QStringLiteral("unknown key '%1' (kept as-is)").arg(fromUtf8(path))));
        }
    }
}

template <class Node> std::string formatToml(const Node& node)
{
    std::ostringstream stream;
    stream << toml::toml_formatter(node, toml::toml_formatter::default_flags & ~toml::format_flags::indentation);
    return stream.str();
}

void appendComment(QString& out, const QString& text)
{
    for (const QString& line : text.split(u'\n')) {
        out += QStringLiteral("# ") + line + u'\n';
    }
}

QString describeConstraints(const Definition& def)
{
    QStringList parts;
    parts.append(QStringLiteral("Type: %1.").arg(latin(typeName(def.type))));
    if (def.type == Type::Enum) {
        QStringList values;
        for (const Option& option : def.options) {
            values.append(u'"' + latin(option.id) + u'"');
        }
        parts.append(QStringLiteral("Values: %1.").arg(values.join(QStringLiteral(" | "))));
    }
    if (def.bounded()) {
        parts.append(QStringLiteral("Range: %1 .. %2.").arg(def.min).arg(def.max));
    }
    if (def.requiresRestart()) {
        parts.append(QStringLiteral("Requires restart."));
    }
    parts.append(QStringLiteral("Env: %1.").arg(Schema::environmentVariable(def)));
    return parts.join(u' ');
}

// Moves the scalar (and inline-table) values of `from` out of it and writes them as "key = value" lines. Tables and
// arrays of tables stay in `from`.
void appendUnknownLeaves(QString& out, toml::table& from)
{
    toml::table leaves;
    std::vector<std::string> moved;
    for (auto&& [key, node] : from) {
        const auto* asTable = node.as_table();
        if ((asTable == nullptr || asTable->is_inline()) && !node.is_array_of_tables()) {
            node.visit([&](auto&& value) { leaves.insert_or_assign(key.str(), value); });
            moved.emplace_back(key.str());
        }
    }
    for (const auto& key : moved) {
        from.erase(key);
    }
    if (!leaves.empty()) {
        out += QStringLiteral("\n# Unrecognised keys, kept as-is:\n") + fromUtf8(formatToml(leaves)) + u'\n';
    }
}

}  // namespace

QString ConfigIssue::logText() const
{
    return QStringLiteral("%1:%2:%3: %4 [%5]").arg(file).arg(line).arg(column).arg(detail, latin(errorCodeName(code)));
}

ParsedConfig parseConfig(std::string_view text, const QString& fileName, const Schema& schema)
{
    ParsedConfig parsed;
    const std::string sourcePath = fileName.toStdString();
    toml::parse_result result = toml::parse(text, sourcePath);
    if (!result) {
        const toml::parse_error& error = result.error();
        parsed.syntaxOk = false;
        parsed.issues.append(ConfigIssue{ConfigIssue::Severity::Error,
                                         ErrorCode::ConfigSyntaxError,
                                         fileName,
                                         static_cast<int>(error.source().begin.line),
                                         static_cast<int>(error.source().begin.column),
                                         {},
                                         fromUtf8(error.description())});
        return parsed;
    }

    toml::table root = std::move(result).table();
    if (const auto* version = root.get(kVersionKey)) {
        if (const auto* number = version->as_integer()) {
            parsed.version = number->get();
        } else {
            parsed.issues.append(issueAt(*version, fileName, ConfigIssue::Severity::Warning,
                                         ErrorCode::ConfigTypeMismatch, latin(kVersionKey),
                                         QStringLiteral("config_version must be an integer")));
        }
    }
    if (parsed.version > Schema::kConfigVersion) {
        parsed.issues.append(ConfigIssue{ConfigIssue::Severity::Warning, ErrorCode::ConfigNewerVersion, fileName, 0, 0,
                                         latin(kVersionKey),
                                         QStringLiteral("file was written by a newer version (%1 > %2)")
                                             .arg(parsed.version)
                                             .arg(Schema::kConfigVersion)});
    }
    migrate(root, parsed.version, Schema::kConfigVersion, builtinMigrations());
    root.erase(kVersionKey);

    // Every settings section, and each table on its path, is written as a [table] header. A value in that place
    // could not be written back next to the header (invalid TOML), so it is dropped; inline tables become
    // standard tables.
    for (const Section& section : schema.sections()) {
        toml::table* current = &root;
        std::string path;
        for (const auto part : splitPath(section.id)) {
            path += path.empty() ? std::string(part) : "." + std::string(part);
            toml::node* node = current->get(part);
            if (node == nullptr) {
                break;
            }
            if (toml::table* child = node->as_table()) {
                child->is_inline(false);
                current = child;
                continue;
            }
            parsed.issues.append(issueAt(*node, fileName, ConfigIssue::Severity::Error, ErrorCode::ConfigTypeMismatch,
                                         latin(path), QStringLiteral("%1: expected a table").arg(latin(path))));
            current->erase(part);
            break;
        }
    }

    for (std::size_t i = 0; i < schema.size(); ++i) {
        const Definition& def = schema.at(i);
        toml::table* parent = tableAt(root, def.sectionId());
        const std::string leaf(def.leafName());
        const toml::node* node = parent != nullptr ? parent->get(leaf) : nullptr;
        if (node == nullptr) {
            continue;
        }
        auto converted = toVariant(def, *node).and_then([&](const QVariant& value) { return validate(def, value); });
        if (converted) {
            parsed.values.emplace_back(i, std::move(*converted));
        } else {
            parsed.issues.append(issueAt(*node, fileName, ConfigIssue::Severity::Error, converted.error().code,
                                         latin(def.key), QString::fromStdString(converted.error().detail)));
        }
        parent->erase(leaf);
    }

    pruneEmpty(root);
    if (!root.empty()) {
        collectUnknownLeaves(root, {}, fileName, parsed.issues);
        parsed.unknownToml = formatToml(root);
    }
    return parsed;
}

QByteArray serializeConfig(const Schema& schema, const ValueMap& userValues, std::string_view unknownToml,
                           std::int64_t version)
{
    toml::table unknown;
    if (!unknownToml.empty()) {
        if (auto reparsed = toml::parse(unknownToml); reparsed) {
            unknown = std::move(reparsed).table();
        }
    }

    std::vector<const QVariant*> user(schema.size(), nullptr);
    for (const auto& [index, value] : userValues) {
        if (index < user.size()) {
            user[index] = &value;
        }
    }

    QString out;
    out.reserve(qsizetype{32} * 1024);
    appendComment(
        out, QStringLiteral(
                 "Noctara configuration.\n"
                 "Every available key is listed below with its description. Lines starting with '#' are defaults;\n"
                 "uncomment and edit to override. Changes are applied live while the app is running.\n"
                 "Precedence: built-in defaults < system config < this file < NOCTARA_* environment < command line.\n"
                 "This file is rewritten when settings change in the app: values and unknown keys are kept,\n"
                 "but your own comments are not. Secrets (tokens, proxy passwords) are never stored here."));
    out += QStringLiteral("\nconfig_version = %1\n").arg(std::max(version, Schema::kConfigVersion));

    // Unknown root-level values go before the first table header; after it they would belong to that table.
    appendUnknownLeaves(out, unknown);

    for (const Section& section : schema.sections()) {
        out += QStringLiteral("\n[%1]\n").arg(latin(section.id));
        appendComment(out, QString::fromUtf8(section.label) + QStringLiteral(" - ") +
                               QString::fromUtf8(section.description));
        for (std::size_t i = 0; i < schema.size(); ++i) {
            const Definition& def = schema.at(i);
            if (def.sectionId() != section.id) {
                continue;
            }
            out += u'\n';
            appendComment(out, def.description == nullptr ? QString::fromUtf8(def.label)
                                                          : QString::fromUtf8(def.label) + QStringLiteral(": ") +
                                                                QString::fromUtf8(def.description));
            appendComment(out, describeConstraints(def));
            const QString leaf = latin(def.leafName());
            if (user[i] != nullptr) {
                out += leaf + QStringLiteral(" = ") + formatTomlValue(def, *user[i]) + u'\n';
            } else {
                out += QStringLiteral("# ") + leaf + QStringLiteral(" = ") + formatTomlValue(def, def.defaultValue) +
                       u'\n';
            }
        }

        // Unknown scalar keys inside a known table stay in that table. Arrays of tables stay in `unknown`: their
        // [[headers]] are written with the full path at the end.
        if (toml::table* extra = tableAt(unknown, section.id)) {
            appendUnknownLeaves(out, *extra);
        }
    }

    pruneEmpty(unknown);
    if (!unknown.empty()) {
        out += QStringLiteral("\n# Unrecognised tables, kept as-is:\n") + fromUtf8(formatToml(unknown)) + u'\n';
    }
    return out.toUtf8();
}

}  // namespace noctara::settings
