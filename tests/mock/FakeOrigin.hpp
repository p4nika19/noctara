#pragma once

// An in-process INetworkTransport for cache tests: serves resources from memory with real HTTP semantics
// (ranges, ETags, 403 for expired URLs, 404, offline), streams bodies in chunks over several event-loop turns
// so cancellation mid-stream is possible, can hold responses back or pause them mid-body, and records every request.

#include "core/Error.hpp"
#include "net/Transport.hpp"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace noctara::test {

class FakeOrigin : public net::INetworkTransport {
public:
    struct Resource {
        QByteArray body;
        QByteArray etag;
        QByteArray type = "application/octet-stream";
        net::Headers headers;
        int status = 200;
    };

    void serve(const QString& url, Resource resource)
    {
        const std::scoped_lock lock(m_mutex);
        m_resources.insert(url, std::move(resource));
    }
    void forbid(const QString& url)
    {
        const std::scoped_lock lock(m_mutex);
        m_forbidden.insert(url);
    }
    void setOffline(bool offline)
    {
        const std::scoped_lock lock(m_mutex);
        m_offline = offline;
    }
    // Streamed bodies are cut (connection error) after this many bytes of the response.
    void failAfter(std::optional<std::int64_t> bytes)
    {
        const std::scoped_lock lock(m_mutex);
        m_failAfter = bytes;
    }
    // Streamed bodies stop after this many bytes of each response; nullopt lets the paused ones go on.
    void pauseAfter(std::optional<std::int64_t> bytes)
    {
        std::vector<std::function<void()>> paused;
        {
            const std::scoped_lock lock(m_mutex);
            m_pauseAfter = bytes;
            if (!bytes) {
                paused = std::exchange(m_paused, {});
            }
        }
        for (auto& run : paused) {
            run();
        }
    }
    void setHold(bool hold)
    {
        std::vector<std::function<void()>> held;
        {
            const std::scoped_lock lock(m_mutex);
            m_hold = hold;
            if (!hold) {
                held = std::exchange(m_held, {});
            }
        }
        for (auto& run : held) {
            run();
        }
    }
    [[nodiscard]] std::vector<net::Request> requests() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_requests;
    }
    [[nodiscard]] int count() const
    {
        const std::scoped_lock lock(m_mutex);
        return static_cast<int>(m_requests.size());
    }
    void reset()
    {
        const std::scoped_lock lock(m_mutex);
        m_requests.clear();
    }

    net::RequestHandle send(net::Request request, QObject* context, net::Callbacks callbacks) override
    {
        auto state = std::make_shared<net::RequestHandle::State>();
        const QPointer<QObject> target(context);
        std::function<void()> start = [this, request, target, callbacks, state] {
            if (target) {
                QMetaObject::invokeMethod(
                    target, [this, request, target, callbacks, state] { begin(request, target, callbacks, state); },
                    Qt::QueuedConnection);
            }
        };
        {
            const std::scoped_lock lock(m_mutex);
            m_requests.push_back(request);
            if (m_hold) {
                m_held.push_back(std::move(start));
                return net::RequestHandle(state);
            }
        }
        start();
        return net::RequestHandle(state);
    }

    static constexpr qsizetype kChunk = 16 * 1024;

private:
    void begin(const net::Request& request, const QPointer<QObject>& target, const net::Callbacks& callbacks,
               const std::shared_ptr<net::RequestHandle::State>& state)
    {
        if (state->cancelled) {
            callbacks.onFinished(fail(ErrorCode::Cancelled, "cancelled"));
            return;
        }
        Resource resource;
        bool found = false;
        std::optional<std::int64_t> failAfter;
        {
            const std::scoped_lock lock(m_mutex);
            if (m_offline) {
                QMetaObject::invokeMethod(
                    target, [callbacks] { callbacks.onFinished(fail(ErrorCode::Offline, "offline")); },
                    Qt::QueuedConnection);
                return;
            }
            const QString url = request.url.toString();
            if (m_forbidden.contains(url)) {
                resource.status = 403;
                found = true;
            } else if (const auto it = m_resources.constFind(url); it != m_resources.cend()) {
                resource = *it;
                found = true;
            }
            failAfter = m_failAfter;
        }
        net::Response response;
        response.url = request.url;
        response.status = found ? resource.status : 404;
        QByteArray body = response.status == 200 ? resource.body : QByteArray();
        if (response.status == 200 && request.range) {
            const auto size = static_cast<std::int64_t>(resource.body.size());
            const std::int64_t first = request.range->first;
            const std::int64_t last = request.range->last < 0 ? size - 1 : std::min(request.range->last, size - 1);
            if (first >= size) {
                response.status = 416;
                body.clear();
            } else {
                response.status = 206;
                body = resource.body.sliced(static_cast<qsizetype>(first), static_cast<qsizetype>(last - first + 1));
                response.headers.append({"content-range", "bytes " + QByteArray::number(first) + '-' +
                                                              QByteArray::number(last) + '/' +
                                                              QByteArray::number(size)});
            }
        }
        response.headers.append({"content-length", QByteArray::number(body.size())});
        if (!resource.etag.isEmpty()) {
            response.headers.append({"etag", resource.etag});
        }
        response.headers.append({"content-type", resource.type});
        response.headers.append(resource.headers.cbegin(), resource.headers.cend());
        if (callbacks.onResponse) {
            callbacks.onResponse(response);
        }
        if (!callbacks.onData || !response.successful()) {
            response.body = body;
            callbacks.onFinished(response);
            return;
        }
        stream(target, callbacks, state, response, body, 0, failAfter);
    }

    void stream(const QPointer<QObject>& target, const net::Callbacks& callbacks,
                const std::shared_ptr<net::RequestHandle::State>& state, const net::Response& response,
                const QByteArray& body, qsizetype offset, std::optional<std::int64_t> failAfter)
    {
        if (state->cancelled) {
            callbacks.onFinished(fail(ErrorCode::Cancelled, "cancelled"));
            return;
        }
        if (failAfter && offset >= *failAfter) {
            callbacks.onFinished(fail(ErrorCode::ConnectionFailed, "connection reset"));
            return;
        }
        if (offset >= body.size()) {
            callbacks.onFinished(response);
            return;
        }
        qsizetype size = std::min(kChunk, body.size() - offset);
        if (failAfter) {
            size = std::min<qsizetype>(size, static_cast<qsizetype>(*failAfter - offset));
        }
        {
            const std::scoped_lock lock(m_mutex);
            if (m_pauseAfter && offset >= *m_pauseAfter) {
                m_paused.emplace_back([this, target, callbacks, state, response, body, offset, failAfter] {
                    if (target) {
                        QMetaObject::invokeMethod(
                            target,
                            [this, target, callbacks, state, response, body, offset, failAfter] {
                                stream(target, callbacks, state, response, body, offset, failAfter);
                            },
                            Qt::QueuedConnection);
                    }
                });
                return;
            }
            if (m_pauseAfter) {
                size = std::min<qsizetype>(size, static_cast<qsizetype>(*m_pauseAfter - offset));
            }
        }
        callbacks.onData(body.sliced(offset, size));
        if (!target) {
            return;
        }
        QMetaObject::invokeMethod(
            target,
            [this, target, callbacks, state, response, body, offset, size, failAfter] {
                stream(target, callbacks, state, response, body, offset + size, failAfter);
            },
            Qt::QueuedConnection);
    }

    mutable std::mutex m_mutex;
    QHash<QString, Resource> m_resources;
    QSet<QString> m_forbidden;
    std::vector<net::Request> m_requests;
    std::vector<std::function<void()>> m_held;
    std::vector<std::function<void()>> m_paused;
    std::optional<std::int64_t> m_pauseAfter;
    std::optional<std::int64_t> m_failAfter;
    bool m_offline = false;
    bool m_hold = false;
};

}  // namespace noctara::test
