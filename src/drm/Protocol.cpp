#include "drm/Protocol.hpp"

#include <QJsonDocument>
#include <QJsonValue>

namespace noctara::drm {

namespace {

// The shared definition, electron/protocol.json, embedded at build time (clang's #embed; the depfile tracks it).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
constexpr char kDefinition[] = {
#embed "../../electron/protocol.json"
    , '\0'};
#pragma clang diagnostic pop

struct Definition {
    int version = 0;
    QJsonObject toHelper;
    QJsonObject fromHelper;
};

const Definition& definition()
{
    static const Definition parsed = [] {
        const QJsonObject root = QJsonDocument::fromJson(QByteArray(kDefinition)).object();
        return Definition{.version = root.value(u"version").toInt(),
                          .toHelper = root.value(u"toHelper").toObject(),
                          .fromHelper = root.value(u"fromHelper").toObject()};
    }();
    return parsed;
}

QString typeName(const QJsonValue& value)
{
    switch (value.type()) {
    case QJsonValue::String: return QStringLiteral("string");
    case QJsonValue::Double: return QStringLiteral("number");
    case QJsonValue::Bool: return QStringLiteral("boolean");
    case QJsonValue::Object: return QStringLiteral("object");
    case QJsonValue::Array: return QStringLiteral("array");
    case QJsonValue::Null: return QStringLiteral("null");
    case QJsonValue::Undefined: break;
    }
    return QStringLiteral("undefined");
}

}  // namespace

int protocolVersion()
{
    return definition().version;
}

QString validate(Direction direction, const QJsonObject& message)
{
    const QJsonObject& types = direction == Direction::ToHelper ? definition().toHelper : definition().fromHelper;
    const QString type = message.value(u"type").toString();
    const QJsonValue fields = types.value(type);
    if (!fields.isObject()) {
        return QStringLiteral("unknown message type \"%1\"").arg(type);
    }
    const QJsonObject spec = fields.toObject();
    for (auto it = spec.begin(); it != spec.end(); ++it) {
        QString expected = it.value().toString();
        const bool optional = expected.endsWith(u'?');
        if (optional) {
            expected.chop(1);
        }
        const QJsonValue value = message.value(it.key());
        if (value.isUndefined()) {
            if (!optional) {
                return QStringLiteral("%1: missing %2").arg(type, it.key());
            }
            continue;
        }
        if (typeName(value) != expected) {
            return QStringLiteral("%1: %2 must be a %3").arg(type, it.key(), expected);
        }
    }
    return {};
}

Result<QJsonObject> decode(QByteArrayView line, Direction direction)
{
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(line.toByteArray(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(ErrorCode::ParseError, "a line that is not a JSON object");
    }
    QJsonObject message = document.object();
    if (const QString problem = validate(direction, message); !problem.isEmpty()) {
        return fail(ErrorCode::InvalidResponse, problem.toStdString());
    }
    return message;
}

QByteArray encode(const QJsonObject& message)
{
    return QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
}

}  // namespace noctara::drm
