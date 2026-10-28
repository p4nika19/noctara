// Shutting down while a download is still in flight: in main.cpp's order (the QML engine, then the cache service,
// then the network service). Run under ASan (debug builds), where a use-after-free crashes and LeakSanitizer
// reports objects nobody deleted.
#include "TestUtil.hpp"
#include "mock/FakeOrigin.hpp"
#include "mock/MockServers.hpp"

#include "cache/CacheService.hpp"
#include "cache/ImageCache.hpp"
#include "net/NetworkService.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"
#include "ui/CoverImageProvider.hpp"

#include <QBuffer>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTemporaryDir>

#include <atomic>
#include <memory>
#include <thread>

using namespace noctara;
using namespace std::chrono_literals;

namespace {

QByteArray pngBytes()
{
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::red);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

}  // namespace

TEST_SUITE("shutdown")
{
    TEST_CASE("the cache and network services shut down with a cover download in flight")
    {
        std::atomic<bool> release{false};
        test::HttpServer origin([&](const test::HttpRequest&) {
            while (!release) {
                std::this_thread::sleep_for(5ms);
            }
            return test::HttpReply{.status = 200, .headers = {{"content-type", "image/png"}}, .body = pngBytes()};
        });
        QTemporaryDir dir;
        settings::SettingsStore store(settings::Schema::builtin());
        REQUIRE(store.setUserValue(settings::keys::ProxyMode, QStringLiteral("none")));
        auto network = std::make_unique<net::NetworkService>(store, net::processEnvironment(), false);
        auto caches = std::make_unique<cache::CacheService>(store, network->transport(), dir.path());

        QObject context;
        bool answered = false;
        caches->images().request(QUrl(QStringLiteral("http://127.0.0.1:%1/cover.png").arg(origin.port())), QSize(8, 8),
                                 &context, [&](const Result<QImage>&) { answered = true; });
        REQUIRE(test::waitUntil([&] { return !origin.requests().empty(); }, 5000));

        caches.reset();
        network.reset();
        release = true;
        test::waitUntil([] { return false; }, 100);  // anything still posted would run (and crash) now
        CHECK_FALSE(answered);
    }

    TEST_CASE("an image://cover request still loading when the engine goes away is cancelled and cleaned up")
    {
        QTemporaryDir dir;
        test::FakeOrigin origin;
        test::FakeOrigin::Resource cover;
        cover.body = pngBytes();
        cover.type = "image/png";
        origin.serve(QStringLiteral("http://origin.test/cover.png"), cover);
        origin.setHold(true);  // the download stays in flight
        settings::SettingsStore store(settings::Schema::builtin());
        cache::CacheService caches(store, origin, dir.path());
        {
            auto engine = std::make_unique<QQmlEngine>();
            // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): QQmlEngine takes ownership of image providers
            engine->addImageProvider(QStringLiteral("cover"), new ui::CoverImageProvider(caches.images()));
            QQmlComponent component(engine.get());
            component.setData(
                "import QtQuick\n"
                "Image { asynchronous: true; source: 'image://cover/http%3A%2F%2Forigin.test%2Fcover.png' }",
                QUrl());
            std::unique_ptr<QObject> image(component.create());
            REQUIRE_MESSAGE(image != nullptr, component.errorString().toStdString());
            REQUIRE(test::waitUntil([&] { return origin.count() == 1; }, 5000));
            image.reset();
            engine.reset();  // cancels the pending response; it must finish so the engine can delete it
        }
        origin.setHold(false);  // the cache's answer now finds no response to deliver to
        test::waitUntil([] { return false; }, 200);
    }
}
