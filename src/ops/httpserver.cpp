// SPDX-License-Identifier: MIT
#include "httpserver.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "util/logging.hpp"
#include "util/threading.hpp"

namespace mxldl::ops
{
    namespace
    {
        char const* statusText(int status)
        {
            switch (status)
            {
                case 200: return "OK";
                case 404: return "Not Found";
                case 405: return "Method Not Allowed";
                case 503: return "Service Unavailable";
                default: return "Unknown";
            }
        }
    }

    HttpServer::HttpServer(int port, HttpHandler handler, std::string name)
        : _port(port)
        , _handler(std::move(handler))
        , _name(std::move(name))
    {}

    HttpServer::~HttpServer()
    {
        stop();
    }

    void HttpServer::start()
    {
        _listenFd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (_listenFd < 0)
        {
            throw std::runtime_error("socket() failed: " + std::string(std::strerror(errno)));
        }
        int const one = 1;
        ::setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(static_cast<uint16_t>(_port));
        if (::bind(_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            ::close(_listenFd);
            _listenFd = -1;
            throw std::runtime_error("bind(" + std::to_string(_port) + ") failed: " + std::string(std::strerror(errno)));
        }
        if (::listen(_listenFd, 16) != 0)
        {
            ::close(_listenFd);
            _listenFd = -1;
            throw std::runtime_error("listen() failed: " + std::string(std::strerror(errno)));
        }

        _running.store(true);
        _thread = std::thread([this] {
            util::setThreadName(_name);
            acceptLoop();
        });
        log::info("http_server_started", {{"name", _name}, {"port", _port}});
    }

    void HttpServer::stop()
    {
        if (!_running.exchange(false))
        {
            return;
        }
        if (_listenFd >= 0)
        {
            ::shutdown(_listenFd, SHUT_RDWR);
            ::close(_listenFd);
            _listenFd = -1;
        }
        if (_thread.joinable())
        {
            _thread.join();
        }
    }

    void HttpServer::acceptLoop()
    {
        while (_running.load())
        {
            pollfd pfd{_listenFd, POLLIN, 0};
            int const rc = ::poll(&pfd, 1, 250);
            if (rc <= 0)
            {
                continue;
            }
            int const fd = ::accept4(_listenFd, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd < 0)
            {
                continue;
            }
            handleConnection(fd);
            ::close(fd);
        }
    }

    void HttpServer::handleConnection(int fd)
    {
        // Read the request head (bounded, with a small timeout).
        timeval tv{};
        tv.tv_sec = 3;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        std::string request;
        char buf[2048];
        while (request.size() < 16384)
        {
            ssize_t const n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0)
            {
                break;
            }
            request.append(buf, static_cast<std::size_t>(n));
            if (request.find("\r\n\r\n") != std::string::npos)
            {
                break;
            }
        }

        HttpResponse resp;
        std::size_t const lineEnd = request.find("\r\n");
        std::string const requestLine = lineEnd == std::string::npos ? request : request.substr(0, lineEnd);
        std::size_t const sp1 = requestLine.find(' ');
        std::size_t const sp2 = requestLine.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos)
        {
            resp = {404, "text/plain; charset=utf-8", "bad request\n"};
        }
        else
        {
            std::string const method = requestLine.substr(0, sp1);
            std::string path = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
            if (auto const q = path.find('?'); q != std::string::npos)
            {
                path.resize(q);
            }
            if (method != "GET" && method != "HEAD")
            {
                resp = {405, "text/plain; charset=utf-8", "method not allowed\n"};
            }
            else
            {
                resp = _handler(path);
                if (method == "HEAD")
                {
                    resp.body.clear();
                }
            }
        }

        std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " " + statusText(resp.status) +
                          "\r\n"
                          "Content-Type: " +
                          resp.contentType +
                          "\r\n"
                          "Content-Length: " +
                          std::to_string(resp.body.size()) +
                          "\r\n"
                          "Connection: close\r\n"
                          "\r\n" +
                          resp.body;
        std::size_t sent = 0;
        while (sent < out.size())
        {
            ssize_t const n = ::send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
            if (n <= 0)
            {
                break;
            }
            sent += static_cast<std::size_t>(n);
        }
    }
}
