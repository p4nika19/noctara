#include "settings/Overrides.hpp"

#include "core/Strings.hpp"

#include <algorithm>

namespace noctara::settings {

namespace {

void assign(OverrideSet& set, std::size_t index, QVariant value)
{
    const auto it = std::ranges::find(set.values, index, &ValueMap::value_type::first);
    if (it != set.values.end()) {
        it->second = std::move(value);  // later flags win
    } else {
        set.values.emplace_back(index, std::move(value));
    }
}

void applyText(OverrideSet& set, const Schema& schema, std::size_t index, QStringView text, const QString& origin)
{
    const Definition& def = schema.at(index);
    if (auto parsed = parseText(def, text)) {
        assign(set, index, std::move(*parsed));
    } else {
        set.issues.append(ConfigIssue{ConfigIssue::Severity::Error, parsed.error().code, origin, 0, 0, latin(def.key),
                                      QString::fromStdString(parsed.error().detail)});
    }
}

void applyAssignment(OverrideSet& set, const Schema& schema, const QString& assignment)
{
    const qsizetype eq = assignment.indexOf(u'=');
    const QString key = assignment.left(eq).trimmed();
    const auto index = eq > 0 ? schema.indexOf(key.toStdString()) : std::nullopt;
    if (!index) {
        set.issues.append(ConfigIssue{ConfigIssue::Severity::Error, ErrorCode::ConfigUnknownKey,
                                      QStringLiteral("--set"), 0, 0, key,
                                      QStringLiteral("expected --set <known.key>=<value>, got '%1'").arg(assignment)});
        return;
    }
    applyText(set, schema, *index, QStringView(assignment).mid(eq + 1), QStringLiteral("--set"));
}

void applyAlias(OverrideSet& set, const Schema& schema, const QString& alias)
{
    const qsizetype eq = alias.indexOf(u'=');
    const QString name = alias.left(eq);
    const auto defs = schema.definitions();
    const auto it = std::ranges::find_if(
        defs, [&](const Definition& def) { return !def.cliAlias.empty() && latin(def.cliAlias) == name; });
    if (eq <= 0 || it == defs.end()) {
        set.issues.append(ConfigIssue{ConfigIssue::Severity::Error, ErrorCode::ConfigUnknownKey,
                                      QStringLiteral("--") + name, 0, 0, name,
                                      QStringLiteral("unknown option '--%1'").arg(alias)});
        return;
    }
    applyText(set, schema, static_cast<std::size_t>(it - defs.begin()), QStringView(alias).mid(eq + 1),
              QStringLiteral("--") + name);
}

}  // namespace

OverrideSet collectEnvironment(const Schema& schema, const QProcessEnvironment& environment)
{
    OverrideSet set;
    for (std::size_t i = 0; i < schema.size(); ++i) {
        const QString name = Schema::environmentVariable(schema.at(i));
        if (environment.contains(name)) {
            applyText(set, schema, i, environment.value(name), name);
        }
    }
    return set;
}

OverrideSet collectCommandLine(const Schema& schema, const QList<CommandLineOverride>& overrides)
{
    OverrideSet set;
    for (const CommandLineOverride& override : overrides) {
        if (override.kind == CommandLineOverride::Kind::Set) {
            applyAssignment(set, schema, override.text);
        } else {
            applyAlias(set, schema, override.text);
        }
    }
    return set;
}

}  // namespace noctara::settings
