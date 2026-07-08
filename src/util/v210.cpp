// SPDX-License-Identifier: MIT
#include "v210.hpp"

#include <cstring>

namespace mxldl::util
{
    void expandUyvyToV210(std::uint8_t const* src, std::size_t srcRowBytes, std::uint8_t* dst, std::size_t dstRowBytes, std::uint32_t width,
        std::uint32_t height)
    {
        // Both UYVY and v210 carry the identical 4:2:2 component sequence
        // (Cb0 Y0 Cr0 Y1 Cb1 Y2 ...); v210 packs three 10-bit components into
        // each little-endian 32-bit word. 8-bit values map to the top bits.
        std::uint32_t const componentsPerRow = width * 2U;

        for (std::uint32_t y = 0; y < height; ++y)
        {
            std::uint8_t const* s = src + static_cast<std::size_t>(y) * srcRowBytes;
            auto* d = reinterpret_cast<std::uint32_t*>(dst + static_cast<std::size_t>(y) * dstRowBytes);

            std::uint32_t c = 0;
            std::uint32_t wordIdx = 0;
            for (; c + 3 <= componentsPerRow; c += 3)
            {
                std::uint32_t const c0 = static_cast<std::uint32_t>(s[c]) << 2;
                std::uint32_t const c1 = static_cast<std::uint32_t>(s[c + 1]) << 2;
                std::uint32_t const c2 = static_cast<std::uint32_t>(s[c + 2]) << 2;
                d[wordIdx++] = c0 | (c1 << 10) | (c2 << 20);
            }
            if (c < componentsPerRow)
            {
                std::uint32_t word = 0;
                for (std::uint32_t k = 0; c + k < componentsPerRow; ++k)
                {
                    word |= (static_cast<std::uint32_t>(s[c + k]) << 2) << (10 * k);
                }
                d[wordIdx++] = word;
            }
            // Zero the 128-byte alignment padding for deterministic payloads.
            std::size_t const written = static_cast<std::size_t>(wordIdx) * 4U;
            if (written < dstRowBytes)
            {
                std::memset(reinterpret_cast<std::uint8_t*>(d) + written, 0, dstRowBytes - written);
            }
        }
    }
}
