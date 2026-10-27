// Prints the settings schema as Markdown (docs/configuration.md) or as a config template.
#include "settings/Schema.hpp"
#include "settings/SchemaDoc.hpp"
#include "settings/TomlCodec.hpp"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>

int main(int argc, char** argv)
{
    const QCoreApplication app(argc, argv);
    const QStringList args = QCoreApplication::arguments();
    const auto& schema = noctara::settings::Schema::builtin();

    QByteArray output;
    if (args.contains(QStringLiteral("--template"))) {
        output = noctara::settings::serializeConfig(schema, {}, {});
    } else {
        output = noctara::settings::schemaMarkdown(schema).toUtf8();
    }
    const auto size = static_cast<std::size_t>(output.size());
    return std::fwrite(output.constData(), 1, size, stdout) == size ? 0 : 1;
}
