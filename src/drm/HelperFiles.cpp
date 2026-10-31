#include "drm/HelperFiles.hpp"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>

#include <array>

namespace noctara::drm {

namespace {

// The helper app and its release pins, embedded at build time (clang's #embed; the depfile tracks the files).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
constexpr char kMain[] = {
#embed "../../electron/main.js"
};
constexpr char kPreload[] = {
#embed "../../electron/preload.js"
};
constexpr char kPlayerHtml[] = {
#embed "../../electron/player.html"
};
constexpr char kPlayerJs[] = {
#embed "../../electron/player.js"
};
constexpr char kProtocolJs[] = {
#embed "../../electron/protocol.js"
};
constexpr char kProtocolJson[] = {
#embed "../../electron/protocol.json"
};
constexpr char kPackage[] = {
#embed "../../electron/package.json"
};
constexpr char kRelease[] = {
#embed "../../electron/helper-release.json"
};
#pragma clang diagnostic pop

constexpr std::array kFiles{
    EmbeddedFile{"main.js", QByteArrayView(kMain, sizeof kMain)},
    EmbeddedFile{"preload.js", QByteArrayView(kPreload, sizeof kPreload)},
    EmbeddedFile{"player.html", QByteArrayView(kPlayerHtml, sizeof kPlayerHtml)},
    EmbeddedFile{"player.js", QByteArrayView(kPlayerJs, sizeof kPlayerJs)},
    EmbeddedFile{"protocol.js", QByteArrayView(kProtocolJs, sizeof kProtocolJs)},
    EmbeddedFile{"protocol.json", QByteArrayView(kProtocolJson, sizeof kProtocolJson)},
    EmbeddedFile{"package.json", QByteArrayView(kPackage, sizeof kPackage)},
};

Artifact artifactFrom(const QJsonObject& object)
{
    return Artifact{.version = object.value(u"version").toString(),
                    .file = object.value(u"file").toString(),
                    .url = QUrl(object.value(u"url").toString()),
                    .sha256 = object.value(u"sha256").toString().toLatin1(),
                    .size = static_cast<std::int64_t>(object.value(u"size").toDouble())};
}

}  // namespace

std::span<const EmbeddedFile> helperAppFiles()
{
    return kFiles;
}

QString helperAppId()
{
    static const QString id = [] {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        for (const EmbeddedFile& file : kFiles) {
            hash.addData(QByteArrayView(file.name));
            hash.addData(file.data);
        }
        return QString::fromLatin1(hash.result().toHex().left(12));
    }();
    return id;
}

const HelperRelease& helperRelease()
{
    static const HelperRelease release = [] {
        const QJsonObject root = QJsonDocument::fromJson(QByteArray(kRelease, sizeof kRelease)).object();
        return HelperRelease{.electron = artifactFrom(root.value(u"electron").toObject()),
                             .hlsjs = artifactFrom(root.value(u"hlsjs").toObject())};
    }();
    return release;
}

}  // namespace noctara::drm
