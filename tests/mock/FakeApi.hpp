#pragma once

// An in-process INetworkTransport for API tests: a handler decides each answer (status, headers, body, or a
// transport error), answers arrive asynchronously on the caller's context thread, and every request is recorded.

#include "core/Error.hpp"
#include "net/Transport.hpp"

#include <QMetaObject>
#include <QPointer>
#include <QUrlQuery>

#include <functional>
#include <mutex>
#include <vector>

namespace noctara::test {

class FakeApi : public net::INetworkTransport {
public:
    using Handler = std::function<Result<net::Response>(const net::Request&)>;

    void setHandler(Handler handler)
    {
        const std::scoped_lock lock(m_mutex);
        m_handler = std::move(handler);
    }

    net::RequestHandle send(net::Request request, QObject* context, net::Callbacks callbacks) override
    {
        Handler handler;
        {
            const std::scoped_lock lock(m_mutex);
            m_requests.push_back(request);
            handler = m_handler;
        }
        Result<net::Response> result = handler ? handler(request) : fail(ErrorCode::ConnectionFailed, "no handler");
        if (result) {
            result->url = request.url;
        }
        QMetaObject::invokeMethod(
            context,
            [guard = QPointer(context), callbacks = std::move(callbacks), result = std::move(result)] {
                if (guard) {
                    callbacks.onFinished(result);
                }
            },
            Qt::QueuedConnection);
        return {};
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

    static net::Response reply(int status, QByteArray body, net::Headers headers = {})
    {
        net::Response response;
        response.status = status;
        response.body = std::move(body);
        response.headers = std::move(headers);
        return response;
    }
    static QString query(const net::Request& request, const QString& name)
    {
        return QUrlQuery(request.url).queryItemValue(name, QUrl::FullyDecoded);
    }
    static QByteArray header(const net::Request& request, QByteArrayView name)
    {
        for (const auto& [key, value] : request.headers) {
            if (key == name) {
                return value;
            }
        }
        return {};
    }

private:
    mutable std::mutex m_mutex;
    Handler m_handler;
    std::vector<net::Request> m_requests;
};

}  // namespace noctara::test
