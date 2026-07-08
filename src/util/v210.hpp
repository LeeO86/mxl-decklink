// SPDX-License-Identifier: MIT
// v210 geometry and pixel format conversion per SPECIFICATION.md §3.3.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mxldl::util
{
    /// v210 row bytes: ceil(width/48) × 128 (§8), identical to DeckLink
    /// bmdFormat10BitYUV row bytes and MXL's v210 line length.
    constexpr std::uint32_t v210RowBytes(std::uint32_t width)
    {
        return ((width + 47U) / 48U) * 128U;
    }

    /// v210a key (alpha) plane line length: 3×10-bit luma samples per 32-bit
    /// LE word, 4-byte line alignment (§3.3).
    constexpr std::uint32_t alpha10RowBytes(std::uint32_t width)
    {
        return ((width + 2U) / 3U) * 4U;
    }

    /// UYVY (bmdFormat8BitYUV) → v210 expansion for one frame (§3.3, only with
    /// CHx_ALLOW_FORMAT_CONVERSION=true). 8-bit values are placed in the top
    /// bits of the 10-bit samples (v = v8 << 2).
    /// \param src        UYVY pixels, 2 bytes per pixel
    /// \param srcRowBytes source stride in bytes (>= width*2)
    /// \param dst        v210 output
    /// \param dstRowBytes destination stride (>= v210RowBytes(width))
    void expandUyvyToV210(std::uint8_t const* src, std::size_t srcRowBytes, std::uint8_t* dst, std::size_t dstRowBytes, std::uint32_t width,
        std::uint32_t height);
}
