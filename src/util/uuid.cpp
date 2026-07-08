// SPDX-License-Identifier: MIT
#include "uuid.hpp"

#include <cstdio>

namespace mxldl::util
{
    namespace
    {
        int hexVal(char c)
        {
            if (c >= '0' && c <= '9')
            {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f')
            {
                return c - 'a' + 10;
            }
            if (c >= 'A' && c <= 'F')
            {
                return c - 'A' + 10;
            }
            return -1;
        }
    }

    std::string Uuid::toString() const
    {
        char buf[37];
        ::snprintf(buf, sizeof(buf), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0], bytes[1], bytes[2], bytes[3],
            bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
        return buf;
    }

    bool Uuid::isNil() const
    {
        for (auto const b : bytes)
        {
            if (b != 0)
            {
                return false;
            }
        }
        return true;
    }

    std::optional<Uuid> parseUuid(std::string_view s)
    {
        if (s.size() != 36)
        {
            return std::nullopt;
        }
        Uuid out{};
        std::size_t byteIdx = 0;
        for (std::size_t i = 0; i < 36;)
        {
            if (i == 8 || i == 13 || i == 18 || i == 23)
            {
                if (s[i] != '-')
                {
                    return std::nullopt;
                }
                ++i;
                continue;
            }
            int const hi = hexVal(s[i]);
            int const lo = hexVal(s[i + 1]);
            if (hi < 0 || lo < 0)
            {
                return std::nullopt;
            }
            out.bytes[byteIdx++] = static_cast<std::uint8_t>((hi << 4) | lo);
            i += 2;
        }
        return out;
    }

    Uuid deriveUuid(Uuid const& base, std::string_view name)
    {
        // FNV-1a-based mixing: deterministic, dependency-free. Collision
        // resistance requirements are trivial here (a handful of format
        // signatures per configured flow UUID).
        Uuid out = base;
        std::uint64_t h1 = 0xcbf29ce484222325ULL;
        std::uint64_t h2 = 0x84222325cbf29ce4ULL;
        auto mix = [](std::uint64_t h, std::uint8_t b) {
            h ^= b;
            return h * 0x100000001b3ULL;
        };
        for (auto const b : base.bytes)
        {
            h1 = mix(h1, b);
            h2 = mix(h2, static_cast<std::uint8_t>(b ^ 0x5a));
        }
        for (auto const c : name)
        {
            h1 = mix(h1, static_cast<std::uint8_t>(c));
            h2 = mix(h2, static_cast<std::uint8_t>(c ^ 0xa5));
        }
        for (int i = 0; i < 8; ++i)
        {
            out.bytes[i] = static_cast<std::uint8_t>(h1 >> (8 * i));
            out.bytes[8 + i] = static_cast<std::uint8_t>(h2 >> (8 * i));
        }
        // RFC 4122 version/variant bits so the result is a well-formed UUID.
        out.bytes[6] = static_cast<std::uint8_t>((out.bytes[6] & 0x0f) | 0x40);
        out.bytes[8] = static_cast<std::uint8_t>((out.bytes[8] & 0x3f) | 0x80);
        return out;
    }
}
