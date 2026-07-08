// SPDX-License-Identifier: MIT
#include <map>
#include <string>

#include <doctest/doctest.h>

#include "config/config.hpp"

using namespace mxldl;

namespace
{
    config::EnvReader envOf(std::map<std::string, std::string> vars)
    {
        return [vars = std::move(vars)](std::string const& name) -> std::optional<std::string> {
            auto const it = vars.find(name);
            if (it == vars.end())
            {
                return std::nullopt;
            }
            return it->second;
        };
    }

    std::map<std::string, std::string> minimalMultiChannel()
    {
        return {
            {"MXL_DECKLINK_CARD_ID", "0xa1b2c3d4"},
            {"CH0_DIRECTION", "input"},
            {"CH0_SUBDEVICE_INDEX", "0"},
            {"CH0_MXL_VIDEO_FLOW_ID", "5fbec3b1-1b0f-417d-9059-8b94a47197ed"},
            {"CH0_MXL_AUDIO_FLOW_ID", "b3bb5be7-9fe9-4324-a5bb-4c70e1084449"},
        };
    }
}

TEST_CASE("minimal multi-channel config parses with defaults")
{
    auto const cfg = config::loadConfig(envOf(minimalMultiChannel()));
    REQUIRE(cfg.channels.size() == 1);
    auto const& ch = cfg.channels[0];
    CHECK(ch.index == 0);
    CHECK(ch.direction == config::Direction::Input);
    CHECK(ch.isAutoMode());
    CHECK(ch.pixelFormat == config::PixelFormat::YUV10);
    CHECK(ch.audioEnable);
    CHECK(ch.audioChannelCount == 16);
    CHECK(ch.audioSampleType == config::AudioSampleType::Int32);
    CHECK(ch.label == "ch0");
    CHECK(ch.groupHint == "ch0");
    CHECK(cfg.cardId == 0xa1b2c3d4);
    CHECK(cfg.domainPath == "/dev/shm/mxl");
    CHECK(cfg.timestampSource == config::TimestampSourceCfg::Hardware);
    CHECK(cfg.healthPort == 9080);
    CHECK(cfg.metricsPort == 9090);
    CHECK(cfg.minHealthyChannels == 1);
    CHECK_FALSE(cfg.legacyMode);
}

TEST_CASE("channel index gaps are allowed (§4)")
{
    auto vars = minimalMultiChannel();
    vars["CH3_DIRECTION"] = "output";
    vars["CH3_SUBDEVICE_INDEX"] = "1";
    vars["CH3_VIDEO_MODE"] = "HD1080p50";
    vars["CH3_MXL_VIDEO_FLOW_ID"] = "0e635152-e501-4d4e-bb87-9f3fe05eb79a";
    vars["CH3_MXL_AUDIO_FLOW_ID"] = "9126cc2f-4c26-4c9b-a6cd-93c4381c9be5";
    auto const cfg = config::loadConfig(envOf(vars));
    REQUIRE(cfg.channels.size() == 2);
    CHECK(cfg.channels[0].index == 0);
    CHECK(cfg.channels[1].index == 3);
    CHECK(cfg.channels[1].direction == config::Direction::Output);
}

TEST_CASE("missing card selector is rejected")
{
    auto vars = minimalMultiChannel();
    vars.erase("MXL_DECKLINK_CARD_ID");
    CHECK_THROWS_AS(config::loadConfig(envOf(vars)), config::ConfigError);
}

TEST_CASE("multiple card selectors are rejected")
{
    auto vars = minimalMultiChannel();
    vars["MXL_DECKLINK_CARD_NAME"] = "DeckLink Duo 2";
    CHECK_THROWS_AS(config::loadConfig(envOf(vars)), config::ConfigError);
}

TEST_CASE("auto video mode is rejected on output channels (§4.3)")
{
    auto vars = minimalMultiChannel();
    vars["CH0_DIRECTION"] = "output";
    CHECK_THROWS_AS(config::loadConfig(envOf(vars)), config::ConfigError);
}

TEST_CASE("unknown video mode is rejected (§3.2.1)")
{
    auto vars = minimalMultiChannel();
    vars["CH0_VIDEO_MODE"] = "HD1080p47";
    CHECK_THROWS_AS(config::loadConfig(envOf(vars)), config::ConfigError);
}

TEST_CASE("8BitYUV requires ALLOW_FORMAT_CONVERSION (§3.3)")
{
    auto vars = minimalMultiChannel();
    vars["CH0_PIXEL_FORMAT"] = "8BitYUV";
    CHECK_THROWS_AS(config::loadConfig(envOf(vars)), config::ConfigError);
    vars["CH0_ALLOW_FORMAT_CONVERSION"] = "true";
    auto const cfg = config::loadConfig(envOf(vars));
    CHECK(cfg.channels[0].pixelFormat == config::PixelFormat::YUV8);
}

TEST_CASE("RGB pixel formats are rejected (§3.3)")
{
    auto vars = minimalMultiChannel();
    vars["CH0_PIXEL_FORMAT"] = "10BitRGB";
    CHECK_THROWS_AS(config::loadConfig(envOf(vars)), config::ConfigError);
}

TEST_CASE("audio flow id required when audio enabled")
{
    auto vars = minimalMultiChannel();
    vars.erase("CH0_MXL_AUDIO_FLOW_ID");
    CHECK_THROWS_AS(config::loadConfig(envOf(vars)), config::ConfigError);
    vars["CH0_AUDIO_ENABLE"] = "false";
    auto const cfg = config::loadConfig(envOf(vars));
    CHECK_FALSE(cfg.channels[0].audioEnable);
}

TEST_CASE("anc flow id required when anc enabled")
{
    auto vars = minimalMultiChannel();
    vars["CH0_VIDEO_ANC_ENABLE"] = "true";
    CHECK_THROWS_AS(config::loadConfig(envOf(vars)), config::ConfigError);
    vars["CH0_MXL_ANC_FLOW_ID"] = "db3bd465-2772-484f-8fac-830b0471258b";
    auto const cfg = config::loadConfig(envOf(vars));
    CHECK(cfg.channels[0].ancEnable);
}

TEST_CASE("duplicate flow UUIDs are rejected (§4.3)")
{
    auto vars = minimalMultiChannel();
    vars["CH1_DIRECTION"] = "input";
    vars["CH1_SUBDEVICE_INDEX"] = "1";
    vars["CH1_MXL_VIDEO_FLOW_ID"] = vars["CH0_MXL_VIDEO_FLOW_ID"];
    vars["CH1_MXL_AUDIO_FLOW_ID"] = "169feb2c-3fae-42a5-ae2e-f6f8cbce29cf";
    CHECK_THROWS_AS(config::loadConfig(envOf(vars)), config::ConfigError);

    // Loopback (an output channel reading a flow produced by an input
    // channel of the same container) is legitimate.
    vars["CH1_DIRECTION"] = "output";
    vars["CH1_VIDEO_MODE"] = "HD1080p50";
    auto const cfg = config::loadConfig(envOf(vars));
    CHECK(cfg.channels.size() == 2);
}

TEST_CASE("same sub-device twice per direction is rejected, but in+out is allowed (§4.3)")
{
    auto vars = minimalMultiChannel();
    vars["CH1_DIRECTION"] = "input";
    vars["CH1_SUBDEVICE_INDEX"] = "0";
    vars["CH1_MXL_VIDEO_FLOW_ID"] = "0e635152-e501-4d4e-bb87-9f3fe05eb79a";
    vars["CH1_MXL_AUDIO_FLOW_ID"] = "169feb2c-3fae-42a5-ae2e-f6f8cbce29cf";
    CHECK_THROWS_AS(config::loadConfig(envOf(vars)), config::ConfigError);

    // Bidirectional sub-device: one input + one output on the same index.
    vars["CH1_DIRECTION"] = "output";
    vars["CH1_VIDEO_MODE"] = "HD1080p50";
    auto const cfg = config::loadConfig(envOf(vars));
    CHECK(cfg.channels.size() == 2);
}

TEST_CASE("invalid UUID is rejected")
{
    auto vars = minimalMultiChannel();
    vars["CH0_MXL_VIDEO_FLOW_ID"] = "not-a-uuid";
    CHECK_THROWS_AS(config::loadConfig(envOf(vars)), config::ConfigError);
}

TEST_CASE("legacy v1.0 variables map to implicit channel 0 (§4.4)")
{
    std::map<std::string, std::string> vars = {
        {"DECKLINK_DEVICE_ID", "0xa1b2c3d4"},
        {"DIRECTION", "input"},
        {"VIDEO_MODE", "HD1080p50"},
        {"MXL_VIDEO_FLOW_ID", "5fbec3b1-1b0f-417d-9059-8b94a47197ed"},
        {"MXL_AUDIO_FLOW_ID", "b3bb5be7-9fe9-4324-a5bb-4c70e1084449"},
        {"AUDIO_CHANNEL_COUNT", "8"},
    };
    auto const cfg = config::loadConfig(envOf(vars));
    CHECK(cfg.legacyMode);
    REQUIRE(cfg.channels.size() == 1);
    auto const& ch = cfg.channels[0];
    CHECK(ch.index == 0);
    CHECK(ch.subdeviceIndex == 0);
    CHECK(ch.videoModeName == "HD1080p50");
    CHECK(ch.audioChannelCount == 8);
    CHECK(cfg.cardId == 0xa1b2c3d4);
}

TEST_CASE("mixed v1.0 and v1.1 variables are rejected (§4.4)")
{
    auto vars = minimalMultiChannel();
    vars["DIRECTION"] = "input";
    CHECK_THROWS_AS(config::loadConfig(envOf(vars)), config::ConfigError);
}

TEST_CASE("min healthy channels must not exceed channel count")
{
    auto vars = minimalMultiChannel();
    vars["MXL_HEALTH_MIN_HEALTHY_CHANNELS"] = "3";
    CHECK_THROWS_AS(config::loadConfig(envOf(vars)), config::ConfigError);
}

TEST_CASE("health and metrics port collision is rejected")
{
    auto vars = minimalMultiChannel();
    vars["HEALTH_PORT"] = "9090";
    CHECK_THROWS_AS(config::loadConfig(envOf(vars)), config::ConfigError);
}

TEST_CASE("effective grain count defaults: 12 HD / 8 UHD (§4.2)")
{
    auto vars = minimalMultiChannel();
    vars["CH0_VIDEO_MODE"] = "HD1080p50";
    auto cfg = config::loadConfig(envOf(vars));
    CHECK(cfg.channels[0].effectiveGrainCount() == 12);

    vars["CH0_VIDEO_MODE"] = "4K2160p50";
    cfg = config::loadConfig(envOf(vars));
    CHECK(cfg.channels[0].effectiveGrainCount() == 8);

    vars["CH0_GRAIN_COUNT"] = "16";
    cfg = config::loadConfig(envOf(vars));
    CHECK(cfg.channels[0].effectiveGrainCount() == 16);
}

TEST_CASE("cpu pin list parsing")
{
    auto vars = minimalMultiChannel();
    vars["MXL_CPU_PIN_LIST"] = "0,2,4-6";
    auto const cfg = config::loadConfig(envOf(vars));
    REQUIRE(cfg.cpuPinList.has_value());
    CHECK(*cfg.cpuPinList == std::vector<int>{0, 2, 4, 5, 6});

    vars["MXL_CPU_PIN_LIST"] = "0,,2";
    CHECK_THROWS_AS(config::loadConfig(envOf(vars)), config::ConfigError);
}

TEST_CASE("invalid enum values are rejected")
{
    auto vars = minimalMultiChannel();
    vars["MXL_TIMESTAMP_SOURCE"] = "gps";
    CHECK_THROWS_AS(config::loadConfig(envOf(vars)), config::ConfigError);

    vars = minimalMultiChannel();
    vars["LOG_LEVEL"] = "verbose";
    CHECK_THROWS_AS(config::loadConfig(envOf(vars)), config::ConfigError);

    vars = minimalMultiChannel();
    vars["CH0_AUDIO_CHANNEL_COUNT"] = "6";
    CHECK_THROWS_AS(config::loadConfig(envOf(vars)), config::ConfigError);

    vars = minimalMultiChannel();
    vars["MXL_DECKLINK_CARD_PROFILE"] = "full";
    CHECK_THROWS_AS(config::loadConfig(envOf(vars)), config::ConfigError);
}
