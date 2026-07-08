// SPDX-License-Identifier: MIT
// Minimal HTTP/1.1 server for health and metrics endpoints (SPECIFICATION.md
// §7.1). Control-plane only; hand-rolled to avoid runtime dependencies.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace mxldl::ops
{
    struct HttpResponse
    {
        int status = 200;
        std::string contentType = "text/plain; charset=utf-8";
        std::string body;
    };

    /// Handler receives the request path (no query parsing needed here).
    using HttpHandler = std::function<HttpResponse(std::string const& path)>;

    class HttpServer
    {
    public:
        HttpServer(int port, HttpHandler handler, std::string name);
        ~HttpServer();

        HttpServer(HttpServer const&) = delete;
        HttpServer& operator=(HttpServer const&) = delete;

        /// Binds and starts the accept loop. Throws std::runtime_error on
        /// bind failure.
        void start();
        void stop();

        [[nodiscard]] int port() const
        {
            return _port;
        }

    private:
        void acceptLoop();
        void handleConnection(int fd);

        int _port;
        HttpHandler _handler;
        std::string _name;
        int _listenFd = -1;
        std::atomic<bool> _running{false};
        std::thread _thread;
    };
}
