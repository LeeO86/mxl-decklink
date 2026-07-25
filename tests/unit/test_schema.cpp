// SPDX-License-Identifier: MIT
#include <algorithm>

#include <doctest/doctest.h>

#include "config/schema.hpp"

using namespace mxldl::config;

TEST_CASE("channel key parsing")
{
    auto const p = parseChannelKey("CH3_DIRECTION");
    REQUIRE(p.has_value());
    CHECK(p->first == 3);
    CHECK(p->second == "DIRECTION");

    auto const p15 = parseChannelKey("CH15_MXL_VIDEO_FLOW_ID");
    REQUIRE(p15.has_value());
    CHECK(p15->first == 15);

    CHECK_FALSE(parseChannelKey("CH16_DIRECTION").has_value()); // §4: x = 0..15
    CHECK_FALSE(parseChannelKey("CHX_DIRECTION").has_value());
    CHECK_FALSE(parseChannelKey("MXL_DOMAIN_PATH").has_value());
    CHECK_FALSE(parseChannelKey("CH1").has_value());
}

TEST_CASE("setting lookup for global and channel keys")
{
    auto const g = lookupSetting("MXL_DOMAIN_PATH");
    REQUIRE(g.has_value());
    CHECK(g->kind == SettingKind::Global);
    CHECK(g->defaultValue == "/dev/shm/mxl");

    auto const c = lookupSetting("CH0_VIDEO_MODE");
    REQUIRE(c.has_value());
    CHECK(c->kind == SettingKind::Channel);
    CHECK(c->type == "enum");
    // 'auto' plus the §3.2.1 table.
    CHECK(c->options.front() == "auto");
    CHECK(std::find(c->options.begin(), c->options.end(), "HD1080p50") != c->options.end());

    CHECK_FALSE(lookupSetting("CH0_NOT_A_KEY").has_value());
    CHECK_FALSE(lookupSetting("NOT_A_KEY").has_value());
}

TEST_CASE("schema covers every §4.1/§4.2 key the loader understands")
{
    // Spot checks that schema and loader stay in sync.
    for (auto const* key : {"MXL_DECKLINK_CARD_ID", "WEB_PORT", "WEB_ENABLE", "MXL_DOMAIN_SCAN_PATH", "MXL_CONFIG_FILE"})
    {
        CAPTURE(key);
        CHECK(lookupSetting(key).has_value());
    }
    CHECK_FALSE(lookupSetting("HEALTH_PORT").has_value());
    CHECK_FALSE(lookupSetting("METRICS_PORT").has_value());
    for (auto const* suffix : {"DIRECTION", "SUBDEVICE_INDEX", "VIDEO_MODE", "MXL_VIDEO_FLOW_ID", "OUTPUT_PREROLL_GRAINS", "LABEL"})
    {
        CAPTURE(suffix);
        CHECK(lookupSetting(std::string("CH7_") + suffix).has_value());
    }
}
