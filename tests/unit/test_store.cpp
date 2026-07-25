// SPDX-License-Identifier: MIT
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

#include <unistd.h>

#include <doctest/doctest.h>

#include "config/store.hpp"

using namespace mxldl;
using namespace mxldl::config;

namespace
{
    struct TempDir
    {
        std::filesystem::path path;

        TempDir()
        {
            path = std::filesystem::temp_directory_path() / ("mxldl-store-" + std::to_string(::getpid()) + "-" + std::to_string(counter()++));
            std::filesystem::create_directories(path);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }

        static int& counter()
        {
            static int c = 0;
            return c;
        }
    };

    EnvReader envOf(std::map<std::string, std::string> vars)
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

    std::map<std::string, std::string> baseEnv(std::string const& configFile)
    {
        return {
            {"MXL_DECKLINK_CARD_ID", "0xa1b2c3d4"},
            {"MXL_CONFIG_FILE", configFile},
            {"CH0_DIRECTION", "input"},
            {"CH0_SUBDEVICE_INDEX", "0"},
            {"CH0_MXL_VIDEO_FLOW_ID", "5fbec3b1-1b0f-417d-9059-8b94a47197ed"},
            {"CH0_MXL_AUDIO_FLOW_ID", "b3bb5be7-9fe9-4324-a5bb-4c70e1084449"},
        };
    }
}

TEST_CASE("missing config file is not an error; store persists on update (§4.5)")
{
    TempDir tmp;
    auto const file = (tmp.path / "cfg.json").string();
    ConfigStore store(envOf(baseEnv(file)));

    CHECK(store.hasFileLayer());
    CHECK_FALSE(std::filesystem::exists(file));
    auto const cfg = store.effectiveConfig();
    CHECK(cfg.channels.size() == 1);

    // Add a channel via the file layer.
    auto const result = store.update({
        {"CH1_DIRECTION", "output"},
        {"CH1_SUBDEVICE_INDEX", "1"},
        {"CH1_VIDEO_MODE", "HD1080p50"},
        {"CH1_MXL_VIDEO_FLOW_ID", "0e635152-e501-4d4e-bb87-9f3fe05eb79a"},
        {"CH1_MXL_AUDIO_FLOW_ID", "9126cc2f-4c26-4c9b-a6cd-93c4381c9be5"},
    });
    REQUIRE(std::holds_alternative<ConfigStore::UpdateResult>(result));
    auto const& updated = std::get<ConfigStore::UpdateResult>(result);
    CHECK(updated.config.channels.size() == 2);
    CHECK(std::filesystem::exists(file));

    // A fresh store must read the same state back.
    ConfigStore store2(envOf(baseEnv(file)));
    CHECK(store2.effectiveConfig().channels.size() == 2);
    CHECK(store2.sourceOf("CH1_DIRECTION") == SettingSource::File);
    CHECK(store2.sourceOf("CH0_DIRECTION") == SettingSource::Env);
    CHECK(store2.sourceOf("LOG_LEVEL") == SettingSource::Default);
}

TEST_CASE("env overrides file and is immutable through the store (§7.5.2)")
{
    TempDir tmp;
    auto const file = (tmp.path / "cfg.json").string();
    {
        std::ofstream out(file);
        out << R"({"CH0_LABEL": "from-file", "LOG_LEVEL": "debug"})";
    }
    auto env = baseEnv(file);
    env["CH0_LABEL"] = "from-env";
    ConfigStore store(envOf(env));

    CHECK(store.effectiveValue("CH0_LABEL") == "from-env");
    CHECK(store.sourceOf("CH0_LABEL") == SettingSource::Env);
    CHECK(store.effectiveValue("LOG_LEVEL") == "debug");
    CHECK(store.effectiveConfig().channels[0].label == "from-env");

    auto const rejected = store.update({{"CH0_LABEL", "changed"}});
    REQUIRE(std::holds_alternative<std::string>(rejected));
    CHECK(std::get<std::string>(rejected).find("environment variable") != std::string::npos);
}

TEST_CASE("invalid merges are rejected atomically (§4.5)")
{
    TempDir tmp;
    auto const file = (tmp.path / "cfg.json").string();
    ConfigStore store(envOf(baseEnv(file)));

    // Duplicate writer UUID with CH0 → §4.3 violation.
    auto const rejected = store.update({
        {"CH2_DIRECTION", "input"},
        {"CH2_SUBDEVICE_INDEX", "2"},
        {"CH2_MXL_VIDEO_FLOW_ID", "5fbec3b1-1b0f-417d-9059-8b94a47197ed"},
        {"CH2_MXL_AUDIO_FLOW_ID", "169feb2c-3fae-42a5-ae2e-f6f8cbce29cf"},
    });
    REQUIRE(std::holds_alternative<std::string>(rejected));
    CHECK_FALSE(std::filesystem::exists(file)); // nothing persisted
    CHECK(store.effectiveConfig().channels.size() == 1);
}

TEST_CASE("unknown file keys are rejected at load (§4.5)")
{
    TempDir tmp;
    auto const file = (tmp.path / "cfg.json").string();
    {
        std::ofstream out(file);
        out << R"({"CH0_TYPO_KEY": "x"})";
    }
    CHECK_THROWS_AS(ConfigStore(envOf(baseEnv(file))), ConfigError);
}

TEST_CASE("unset removes file keys; env block renders the effective config")
{
    TempDir tmp;
    auto const file = (tmp.path / "cfg.json").string();
    ConfigStore store(envOf(baseEnv(file)));

    auto r1 = store.update({{"CH0_LABEL", "cam-1"}});
    REQUIRE(std::holds_alternative<ConfigStore::UpdateResult>(r1));
    CHECK(store.effectiveValue("CH0_LABEL") == "cam-1");
    CHECK(store.renderEnvBlock().find("CH0_LABEL=cam-1\n") != std::string::npos);
    CHECK(store.renderEnvBlock().find("MXL_DECKLINK_CARD_ID=0xa1b2c3d4\n") != std::string::npos);

    auto r2 = store.update({{"CH0_LABEL", std::nullopt}});
    REQUIRE(std::holds_alternative<ConfigStore::UpdateResult>(r2));
    CHECK_FALSE(store.effectiveValue("CH0_LABEL").has_value());
}

TEST_CASE("no file layer: updates are rejected with a helpful message")
{
    auto env = baseEnv("");
    env.erase("MXL_CONFIG_FILE");
    ConfigStore store(envOf(env));
    CHECK_FALSE(store.hasFileLayer());
    auto const rejected = store.update({{"CH0_LABEL", "x"}});
    REQUIRE(std::holds_alternative<std::string>(rejected));
    CHECK(std::get<std::string>(rejected).find("MXL_CONFIG_FILE") != std::string::npos);
}

TEST_CASE("global part comparison drives restart_required (§7.5.3)")
{
    TempDir tmp;
    auto const file = (tmp.path / "cfg.json").string();
    ConfigStore store(envOf(baseEnv(file)));
    auto const active = store.effectiveConfig();

    // Channel-only change: global part equal.
    auto r1 = store.update({{"CH0_LABEL", "cam-1"}});
    REQUIRE(std::holds_alternative<ConfigStore::UpdateResult>(r1));
    CHECK(globalPartEquals(std::get<ConfigStore::UpdateResult>(r1).config, active));

    // Global change: differs.
    auto r2 = store.update({{"SIGNAL_LOSS_TIMEOUT_S", "60"}});
    REQUIRE(std::holds_alternative<ConfigStore::UpdateResult>(r2));
    CHECK_FALSE(globalPartEquals(std::get<ConfigStore::UpdateResult>(r2).config, active));
}
