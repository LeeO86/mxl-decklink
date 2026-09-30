// SPDX-License-Identifier: MIT
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

#include <unistd.h>

#include <doctest/doctest.h>

#include "config/config.hpp"
#include "mxlbridge/domainscan.hpp"
#include "nmos/routing.hpp"
#include "util/uuid.hpp"

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
}

TEST_CASE("NMOS legs are stable and activations retarget one MXL flow")
{
    auto const cfg = config::loadConfig(envOf({
        {"MXL_DECKLINK_CARD_ID", "0xa1b2c3d4"},
        {"CH0_DIRECTION", "input"},
        {"CH0_SUBDEVICE_INDEX", "0"},
        {"CH0_VIDEO_MODE", "HD1080p50"},
        {"CH0_MXL_VIDEO_FLOW_ID", "5fbec3b1-1b0f-417d-9059-8b94a47197ed"},
        {"CH0_AF0_FLOW_ID", "b3bb5be7-9fe9-4324-a5bb-4c70e1084449"},
        {"CH0_AF0_CHANNEL_COUNT", "2"},
        {"CH0_AF0_MAP", "0,1"},
        {"CH1_DIRECTION", "output"},
        {"CH1_SUBDEVICE_INDEX", "1"},
        {"CH1_VIDEO_MODE", "HD1080p50"},
        {"CH1_MXL_VIDEO_FLOW_ID", "00000000-0000-0000-0000-000000000000"},
        {"CH1_AF0_FLOW_ID", "00000000-0000-0000-0000-000000000000"},
        {"CH1_AF0_CHANNEL_COUNT", "2"},
        {"CH1_AF0_MAP", "0,1"},
    }));
    constexpr std::uint32_t card = 0xa1b2c3d4;
    auto const legs = nmosroute::enumerateLegs(cfg, card);
    REQUIRE(legs.size() == 4);
    CHECK(legs[0].sender);
    CHECK(legs[2].sender == false);
    CHECK(nmosroute::enumerateLegs(cfg, card)[0].id == legs[0].id);
    CHECK(nmosroute::nodeIdForCard(card) == nmosroute::stableId(card, "node"));

    auto const domainDir = std::filesystem::temp_directory_path() / ("mxldl-nmos-dom-" + std::to_string(::getpid()));
    std::filesystem::create_directories(domainDir);
    std::vector<nmosroute::DomainBinding> domains{{"11111111-1111-4111-8111-111111111111", domainDir.string()}};
    auto channels = cfg.channels;

    nmosroute::Activation enableReceiver;
    enableReceiver.legId = legs[2].id;
    enableReceiver.masterEnable = true;
    enableReceiver.domainId = domains[0].id;
    enableReceiver.flowId = "5fbec3b1-1b0f-417d-9059-8b94a47197ed";
    CHECK_FALSE(nmosroute::applyActivation(channels, legs, enableReceiver, domains, domainDir.string()).has_value());
    CHECK(channels[1].videoMxlActive);
    CHECK(channels[1].videoFlowId.toString() == "5fbec3b1-1b0f-417d-9059-8b94a47197ed");
    CHECK(channels[1].readerDomainPath.empty());

    nmosroute::Activation disableSender = enableReceiver;
    disableSender.legId = legs[0].id;
    disableSender.masterEnable = false;
    disableSender.flowId.reset();
    CHECK_FALSE(nmosroute::applyActivation(channels, legs, disableSender, domains, domainDir.string()).has_value());
    CHECK_FALSE(channels[0].videoMxlActive);
    CHECK(channels[0].videoFlowId.toString() == "5fbec3b1-1b0f-417d-9059-8b94a47197ed");

    nmosroute::Activation missing;
    missing.legId = legs[2].id;
    missing.masterEnable = true;
    missing.domainId = "22222222-2222-4222-8222-222222222222";
    missing.flowId = "5fbec3b1-1b0f-417d-9059-8b94a47197ed";
    CHECK(nmosroute::applyActivation(channels, legs, missing, domains, domainDir.string()).has_value());
    std::filesystem::remove_all(domainDir);
}

TEST_CASE("ensureDomainId writes a stable domain_def.json")
{
    namespace fs = std::filesystem;
    auto const root = fs::temp_directory_path() / ("mxldl-domain-" + std::to_string(::getpid()));
    fs::create_directories(root);
    auto const first = mxlbridge::ensureDomainId(root.string());
    auto const second = mxlbridge::ensureDomainId(root.string());
    CHECK(first == second);
    CHECK(util::parseUuid(first).has_value());
    std::ifstream in(root / "domain_def.json");
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(body.find("\"tags\"") != std::string::npos);
    fs::remove_all(root);
}