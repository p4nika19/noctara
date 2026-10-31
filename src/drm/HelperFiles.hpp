#pragma once

#include <QByteArrayView>
#include <QString>
#include <QUrl>

#include <cstdint>
#include <span>

namespace noctara::drm {

// One file of the helper app (electron/), compiled into Noctara so its own installer can write it out.
struct EmbeddedFile {
    const char* name;  // relative to the app folder
    QByteArrayView data;
};

// What the helper runs on, pinned in electron/helper-release.json (shared with `xmake drm-helper`).
struct Artifact {
    QString version;
    QString file;
    QUrl url;
    QByteArray sha256;  // lower-case hex
    std::int64_t size = 0;
};

struct HelperRelease {
    Artifact electron;  // castlabs Electron with Widevine, a zip
    Artifact hlsjs;     // hls.js, one script
};

[[nodiscard]] std::span<const EmbeddedFile> helperAppFiles();
// A short hash of the app files: names the folder they are installed to, so a changed app gets a new folder.
[[nodiscard]] QString helperAppId();
[[nodiscard]] const HelperRelease& helperRelease();

}  // namespace noctara::drm
