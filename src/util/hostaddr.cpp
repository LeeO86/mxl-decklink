// SPDX-License-Identifier: MIT
#include "hostaddr.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>

namespace mxldl::util
{
    std::optional<std::string> firstNonLoopbackIpv4()
    {
        ifaddrs* list = nullptr;
        if (::getifaddrs(&list) != 0)
        {
            return std::nullopt;
        }
        std::optional<std::string> found;
        for (auto* it = list; it != nullptr; it = it->ifa_next)
        {
            if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET)
            {
                continue;
            }
            auto const* addr = reinterpret_cast<sockaddr_in const*>(it->ifa_addr);
            char buf[INET_ADDRSTRLEN] = {};
            if (::inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf)) == nullptr)
            {
                continue;
            }
            std::string const ip(buf);
            if (ip.rfind("127.", 0) == 0)
            {
                continue;
            }
            found = ip;
            break;
        }
        ::freeifaddrs(list);
        return found;
    }

    bool isIpLiteral(std::string const& text)
    {
        in_addr v4{};
        in6_addr v6{};
        return ::inet_pton(AF_INET, text.c_str(), &v4) == 1 || ::inet_pton(AF_INET6, text.c_str(), &v6) == 1;
    }
}
