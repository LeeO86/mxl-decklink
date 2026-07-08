// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <DeckLinkAPI.h>

#include "config/videomodes.hpp"

using namespace mxldl::config;

TEST_CASE("essential §3.2.1 mode mappings")
{
    struct Expect
    {
        char const* name;
        std::uint32_t bmd;
        std::uint32_t w, h;
        std::int64_t num, den;
        bool interlaced;
    };
    Expect const cases[] = {
        {"HD720p50", bmdModeHD720p50, 1280, 720, 50, 1, false},
        {"HD720p5994", bmdModeHD720p5994, 1280, 720, 60000, 1001, false},
        {"HD1080i50", bmdModeHD1080i50, 1920, 1080, 25, 1, true},
        {"HD1080p24", bmdModeHD1080p24, 1920, 1080, 24, 1, false},
        {"HD1080p25", bmdModeHD1080p25, 1920, 1080, 25, 1, false},
        {"HD1080p2997", bmdModeHD1080p2997, 1920, 1080, 30000, 1001, false},
        {"HD1080p50", bmdModeHD1080p50, 1920, 1080, 50, 1, false},
        {"HD1080p5994", bmdModeHD1080p5994, 1920, 1080, 60000, 1001, false},
        {"4K2160p25", bmdMode4K2160p25, 3840, 2160, 25, 1, false},
        {"4K2160p50", bmdMode4K2160p50, 3840, 2160, 50, 1, false},
        {"4K2160p5994", bmdMode4K2160p5994, 3840, 2160, 60000, 1001, false},
        {"4K2160p60", bmdMode4K2160p60, 3840, 2160, 60, 1, false},
        {"8K4320p50", bmdMode8K4320p50, 7680, 4320, 50, 1, false},
        {"8K4320p60", bmdMode8K4320p60, 7680, 4320, 60, 1, false},
    };
    for (auto const& c : cases)
    {
        CAPTURE(c.name);
        auto const m = lookupVideoMode(c.name);
        REQUIRE(m.has_value());
        CHECK(m->bmdDisplayMode == c.bmd);
        CHECK(m->width == c.w);
        CHECK(m->height == c.h);
        CHECK(m->rateNumerator == c.num);
        CHECK(m->rateDenominator == c.den);
        CHECK(m->interlaced == c.interlaced);
    }
}

TEST_CASE("modes not in the table are rejected (§3.2.1)")
{
    CHECK_FALSE(lookupVideoMode("HD1080p47").has_value());
    CHECK_FALSE(lookupVideoMode("2kDCI25").has_value());
    CHECK_FALSE(lookupVideoMode("").has_value());
}

TEST_CASE("reverse lookup by BMDDisplayMode")
{
    auto const m = lookupVideoModeByBmd(bmdModeHD720p50);
    REQUIRE(m.has_value());
    CHECK(m->name == "HD720p50");
    CHECK_FALSE(lookupVideoModeByBmd(0x12345678).has_value());
}
