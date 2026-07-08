// SPDX-License-Identifier: MIT
#include <vector>

#include <doctest/doctest.h>

#include "util/v210.hpp"

using namespace mxldl::util;

TEST_CASE("v210 row bytes math (§8: ceil(width/48) × 128)")
{
    CHECK(v210RowBytes(1920) == 5120);
    CHECK(v210RowBytes(1280) == 3456); // ceil(1280/48)=27 → 3456
    CHECK(v210RowBytes(3840) == 10240);
    CHECK(v210RowBytes(7680) == 20480);
    CHECK(v210RowBytes(720) == 1920);
    CHECK(v210RowBytes(1) == 128);
}

TEST_CASE("1080p50 v210 frame size matches the spec's 5.27 MiB estimate (§6.3)")
{
    CHECK(v210RowBytes(1920) * 1080 == 5'529'600);
}

TEST_CASE("alpha plane row bytes (§3.3: 3×10-bit per 32-bit word)")
{
    CHECK(alpha10RowBytes(1920) == 2560);
    CHECK(alpha10RowBytes(3) == 4);
    CHECK(alpha10RowBytes(4) == 8);
}

TEST_CASE("UYVY→v210 expansion golden vector")
{
    // 6 pixels, 1 row: UYVY components 0x10..0x1B.
    std::uint32_t const width = 6;
    std::uint32_t const height = 1;
    std::vector<std::uint8_t> src(width * 2);
    for (std::size_t i = 0; i < src.size(); ++i)
    {
        src[i] = static_cast<std::uint8_t>(0x10 + i);
    }
    std::vector<std::uint8_t> dst(v210RowBytes(width), 0xff);
    expandUyvyToV210(src.data(), src.size(), dst.data(), dst.size(), width, height);

    auto const* words = reinterpret_cast<std::uint32_t const*>(dst.data());
    // Each component is expanded 8→10 bits (v = v8 << 2), 3 per word.
    auto expectWord = [&](std::size_t w, std::uint8_t c0, std::uint8_t c1, std::uint8_t c2) {
        std::uint32_t const expected =
            (static_cast<std::uint32_t>(c0) << 2) | ((static_cast<std::uint32_t>(c1) << 2) << 10) | ((static_cast<std::uint32_t>(c2) << 2) << 20);
        CHECK(words[w] == expected);
    };
    expectWord(0, 0x10, 0x11, 0x12);
    expectWord(1, 0x13, 0x14, 0x15);
    expectWord(2, 0x16, 0x17, 0x18);
    expectWord(3, 0x19, 0x1a, 0x1b);
    // Alignment padding must be zeroed.
    for (std::size_t i = 16; i < dst.size(); ++i)
    {
        CHECK(dst[i] == 0);
    }
}

TEST_CASE("UYVY→v210 handles widths not divisible by 6 (e.g. 1280)")
{
    std::uint32_t const width = 8; // 16 components → 5 words + remainder
    std::vector<std::uint8_t> src(width * 2, 0x80);
    std::vector<std::uint8_t> dst(v210RowBytes(width));
    expandUyvyToV210(src.data(), src.size(), dst.data(), dst.size(), width, 1);
    auto const* words = reinterpret_cast<std::uint32_t const*>(dst.data());
    // 16 components = 5 full words + 1 component in the 6th word.
    std::uint32_t const c = 0x80U << 2;
    CHECK(words[0] == (c | (c << 10) | (c << 20)));
    CHECK(words[4] == (c | (c << 10) | (c << 20)));
    CHECK(words[5] == c);
}
