// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string>

namespace mxldl::util
{
    /// First non-loopback IPv4 address on this host, or nullopt when none exists.
    [[nodiscard]] std::optional<std::string> firstNonLoopbackIpv4();

    /// True when `text` is an IPv4 or IPv6 address literal.
    [[nodiscard]] bool isIpLiteral(std::string const& text);
}
