// SPDX-License-Identifier: MIT
#include "uuid.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

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

    namespace
    {
        std::uint32_t rotl(std::uint32_t v, int n)
        {
            return (v << n) | (v >> (32 - n));
        }

        void sha1(std::uint8_t const* data, std::size_t len, std::uint8_t out[20])
        {
            std::uint32_t h0 = 0x67452301u;
            std::uint32_t h1 = 0xEFCDAB89u;
            std::uint32_t h2 = 0x98BADCFEu;
            std::uint32_t h3 = 0x10325476u;
            std::uint32_t h4 = 0xC3D2E1F0u;
            std::uint64_t const bits = static_cast<std::uint64_t>(len) * 8u;
            std::vector<std::uint8_t> msg(data, data + len);
            msg.push_back(0x80);
            while ((msg.size() % 64) != 56)
            {
                msg.push_back(0);
            }
            for (int i = 7; i >= 0; --i)
            {
                msg.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
            }
            for (std::size_t off = 0; off < msg.size(); off += 64)
            {
                std::uint32_t w[80];
                for (int i = 0; i < 16; ++i)
                {
                    w[i] = (std::uint32_t(msg[off + i * 4]) << 24) | (std::uint32_t(msg[off + i * 4 + 1]) << 16) |
                           (std::uint32_t(msg[off + i * 4 + 2]) << 8) | std::uint32_t(msg[off + i * 4 + 3]);
                }
                for (int i = 16; i < 80; ++i)
                {
                    w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
                }
                std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
                for (int i = 0; i < 80; ++i)
                {
                    std::uint32_t f = 0;
                    std::uint32_t k = 0;
                    if (i < 20)
                    {
                        f = (b & c) | ((~b) & d);
                        k = 0x5A827999u;
                    }
                    else if (i < 40)
                    {
                        f = b ^ c ^ d;
                        k = 0x6ED9EBA1u;
                    }
                    else if (i < 60)
                    {
                        f = (b & c) | (b & d) | (c & d);
                        k = 0x8F1BBCDCu;
                    }
                    else
                    {
                        f = b ^ c ^ d;
                        k = 0xCA62C1D6u;
                    }
                    std::uint32_t const temp = rotl(a, 5) + f + e + k + w[i];
                    e = d;
                    d = c;
                    c = rotl(b, 30);
                    b = a;
                    a = temp;
                }
                h0 += a;
                h1 += b;
                h2 += c;
                h3 += d;
                h4 += e;
            }
            std::uint32_t const hs[5] = {h0, h1, h2, h3, h4};
            for (int i = 0; i < 5; ++i)
            {
                out[i * 4] = static_cast<std::uint8_t>(hs[i] >> 24);
                out[i * 4 + 1] = static_cast<std::uint8_t>(hs[i] >> 16);
                out[i * 4 + 2] = static_cast<std::uint8_t>(hs[i] >> 8);
                out[i * 4 + 3] = static_cast<std::uint8_t>(hs[i]);
            }
        }
    }

    Uuid uuidNamespaceDns()
    {
        return *parseUuid("6ba7b810-9dad-11d1-80b4-00c04fd430c8");
    }

    Uuid uuidV5(Uuid const& ns, std::string_view name)
    {
        std::vector<std::uint8_t> buf(ns.bytes.begin(), ns.bytes.end());
        buf.insert(buf.end(), name.begin(), name.end());
        std::uint8_t dig[20];
        sha1(buf.data(), buf.size(), dig);
        Uuid out{};
        std::memcpy(out.bytes.data(), dig, 16);
        out.bytes[6] = static_cast<std::uint8_t>((out.bytes[6] & 0x0f) | 0x50);
        out.bytes[8] = static_cast<std::uint8_t>((out.bytes[8] & 0x3f) | 0x80);
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
