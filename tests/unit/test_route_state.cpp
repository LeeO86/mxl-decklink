// SPDX-License-Identifier: MIT
// IS-05 activations survive a restart (SPECIFICATION.md §7.7): saved to
// state/connections.json and applied again before the channels start.
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

#include <unistd.h>

#include <doctest/doctest.h>

#include "config/config.hpp"
#include "nmos/route_state.hpp"
#include "nmos/routing.hpp"

using namespace mxldl;
namespace fs = std::filesystem;

namespace
{
    constexpr std::uint32_t kCard = 0xa1b2c3d4;
    constexpr char const* kVideoFlow = "5fbec3b1-1b0f-417d-9059-8b94a47197ed";
    constexpr char const* kAudioFlow = "b3bb5be7-9fe9-4324-a5bb-4c70e1084449";
    constexpr char const* kPrimaryId = "00000000-0000-4000-8000-0000000000aa";
    constexpr char const* kDomainA = "11111111-1111-4111-8111-111111111111"; // video's function
    constexpr char const* kDomainB = "22222222-2222-4222-8222-222222222222"; // audio's function

    config::Config loadTestConfig(std::string const& configDir)
    {
        std::map<std::string, std::string> vars{
            {"MXL_DECKLINK_CARD_ID", "0xa1b2c3d4"},
            {"CONFIG_DIR", configDir},
            {"CH0_DIRECTION", "input"},
            {"CH0_SUBDEVICE_INDEX", "0"},
            {"CH0_VIDEO_MODE", "HD1080p50"},
            {"CH0_MXL_VIDEO_FLOW_ID", "0f0f0f0f-1b0f-417d-9059-8b94a47197ed"},
            {"CH0_AF0_FLOW_ID", "0e0e0e0e-9fe9-4324-a5bb-4c70e1084449"},
            {"CH0_AF0_CHANNEL_COUNT", "2"},
            {"CH0_AF0_MAP", "0,1"},
            {"CH1_DIRECTION", "output"},
            {"CH1_SUBDEVICE_INDEX", "1"},
            {"CH1_VIDEO_MODE", "HD1080p50"},
            {"CH1_MXL_VIDEO_FLOW_ID", "00000000-0000-0000-0000-000000000000"},
            {"CH1_AF0_FLOW_ID", "00000000-0000-0000-0000-000000000000"},
            {"CH1_AF0_CHANNEL_COUNT", "2"},
            {"CH1_AF0_MAP", "0,1"},
        };
        return config::loadConfig([vars](std::string const& name) -> std::optional<std::string> {
            auto const it = vars.find(name);
            return it == vars.end() ? std::nullopt : std::optional<std::string>(it->second);
        });
    }

    struct Dirs
    {
        fs::path root = fs::temp_directory_path() / ("mxldl-routes-" + std::to_string(::getpid()));
        fs::path config = root / "config";
        fs::path primary = root / "decklink";
        fs::path domainA = root / "player";
        fs::path domainB = root / "audio-mixer";

        Dirs()
        {
            fs::remove_all(root);
            for (auto const& d : {config, primary, domainA, domainB})
            {
                fs::create_directories(d);
            }
        }

        ~Dirs()
        {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    };

    nmosroute::Leg const& legOf(std::vector<nmosroute::Leg> const& legs, bool sender, nmosroute::LegKind kind)
    {
        for (auto const& leg : legs)
        {
            if (leg.sender == sender && leg.kind == kind)
            {
                return leg;
            }
        }
        throw std::runtime_error("no such leg");
    }
}

TEST_CASE("route state file: saved activations load again, a broken file is ignored")
{
    Dirs dirs;
    auto const cfg = loadTestConfig(dirs.config.string());
    auto const path = nmosroute::routeStatePath(cfg);
    CHECK(path == (dirs.config / "state" / "connections.json").string());

    {
        nmosroute::RouteState state(path);
        state.load(); // first start: no file
        CHECK(state.all().empty());
        nmosroute::SavedActivation a;
        a.legId = "6a4c3e1e-8a43-5e8e-9b2f-0b1d2c3e4f50";
        a.masterEnable = true;
        a.domainId = kDomainA;
        a.domainPath = dirs.domainA.string();
        a.flowId = kVideoFlow;
        a.peerId = "7b5d4f2f-9b54-5f9f-8c3a-1c2e3d4f5061";
        CHECK(state.save(a));
        nmosroute::SavedActivation off;
        off.legId = "6a4c3e1e-8a43-5e8e-9b2f-0b1d2c3e4f51";
        off.sender = true;
        CHECK(state.save(off));
    }

    nmosroute::RouteState again(path);
    again.load();
    REQUIRE(again.all().size() == 2);
    auto const a = again.find("6a4c3e1e-8a43-5e8e-9b2f-0b1d2c3e4f50");
    REQUIRE(a);
    CHECK_FALSE(a->sender);
    CHECK(a->masterEnable);
    CHECK(a->domainId == kDomainA);
    CHECK(a->domainPath == dirs.domainA.string());
    CHECK(a->flowId == std::optional<std::string>(kVideoFlow));
    CHECK(a->peerId == std::optional<std::string>("7b5d4f2f-9b54-5f9f-8c3a-1c2e3d4f5061"));
    auto const off = again.find("6a4c3e1e-8a43-5e8e-9b2f-0b1d2c3e4f51");
    REQUIRE(off);
    CHECK(off->sender);
    CHECK_FALSE(off->masterEnable);
    CHECK_FALSE(off->flowId);
    CHECK_FALSE(off->peerId);

    // A broken file is set aside as connections.json.bad, not overwritten by
    // the next save with one route.
    {
        std::ofstream broken(path, std::ios::trunc);
        broken << "{ not json";
    }
    nmosroute::RouteState invalid(path);
    invalid.load();
    CHECK(invalid.all().empty());
    CHECK_FALSE(fs::exists(path));
    REQUIRE(fs::exists(path + ".bad"));
    {
        std::ifstream kept(path + ".bad");
        std::string text((std::istreambuf_iterator<char>(kept)), std::istreambuf_iterator<char>());
        CHECK(text == "{ not json");
    }
    nmosroute::SavedActivation one;
    one.legId = "6a4c3e1e-8a43-5e8e-9b2f-0b1d2c3e4f52";
    CHECK(invalid.save(one));
    CHECK(fs::exists(path));
    CHECK_FALSE(fs::exists(path + ".tmp"));

    // So is an empty one.
    fs::remove(path + ".bad");
    { std::ofstream empty(path, std::ios::trunc); }
    nmosroute::RouteState emptyState(path);
    emptyState.load();
    CHECK(emptyState.all().empty());
    CHECK(fs::exists(path + ".bad"));
}

TEST_CASE("a restart restores the IS-05 routes of an output with video and audio from different domains")
{
    Dirs dirs;
    std::vector<nmosroute::DomainBinding> const domains{
        {kPrimaryId, dirs.primary.string()},
        {kDomainA, dirs.domainA.string()},
        {kDomainB, dirs.domainB.string()},
    };

    // First run: the controller routes the output's video from domain A and
    // its audio from domain B, and switches the input's audio sender off.
    // The node applies and saves each activation.
    std::vector<config::ChannelConfig> firstRun;
    {
        auto const cfg = loadTestConfig(dirs.config.string());
        auto const legs = nmosroute::enumerateLegs(cfg, kCard);
        nmosroute::RouteState state(nmosroute::routeStatePath(cfg));
        state.load();
        firstRun = cfg.channels;

        auto activate = [&](nmosroute::Leg const& leg, bool enable, char const* domainId, std::optional<std::string> flow, char const* domainPath) {
            nmosroute::Activation a;
            a.legId = leg.id;
            a.masterEnable = enable;
            a.domainId = domainId;
            a.flowId = flow;
            REQUIRE_FALSE(nmosroute::applyActivation(firstRun, legs, a, domains, dirs.primary.string()).has_value());
            nmosroute::SavedActivation saved;
            saved.legId = leg.id;
            saved.sender = leg.sender;
            saved.masterEnable = enable;
            saved.domainId = domainId;
            saved.domainPath = domainPath;
            saved.flowId = flow;
            REQUIRE(state.save(saved));
        };
        activate(legOf(legs, false, nmosroute::LegKind::Video), true, kDomainA, kVideoFlow, dirs.domainA.string().c_str());
        activate(legOf(legs, false, nmosroute::LegKind::Audio), true, kDomainB, kAudioFlow, dirs.domainB.string().c_str());
        activate(legOf(legs, true, nmosroute::LegKind::Audio), false, kPrimaryId, std::nullopt, dirs.primary.string().c_str());

        auto const& out = firstRun[1];
        CHECK(out.videoMxlActive);
        CHECK(out.videoFlowId.toString() == kVideoFlow);
        CHECK(out.readerDomainPath == dirs.domainA.string());
        CHECK(out.audioFlows[0].flowId.toString() == kAudioFlow);
        CHECK(out.audioFlows[0].readerDomainPath == dirs.domainB.string());
        CHECK_FALSE(firstRun[0].audioFlows[0].mxlActive);
    }

    // Restart: the environment still says nil flows; the saved routes win.
    auto const cfg = loadTestConfig(dirs.config.string());
    CHECK(cfg.channels[1].videoFlowId.isNil());
    auto const legs = nmosroute::enumerateLegs(cfg, kCard);
    nmosroute::RouteState state(nmosroute::routeStatePath(cfg));
    state.load();
    auto channels = cfg.channels;
    auto const outcomes = nmosroute::restoreActivations(channels, legs, state.all(), domains, kPrimaryId, dirs.primary.string());
    REQUIRE(outcomes.size() == 3);
    for (auto const& o : outcomes)
    {
        // The flows are not there (no writer in this test): the receivers
        // wait for them; nothing is disabled.
        CHECK(o.kind != nmosroute::RestoreOutcome::Kind::Disabled);
        CHECK(o.kind != nmosroute::RestoreOutcome::Kind::Skipped);
    }
    CHECK(channels == firstRun);
}

TEST_CASE("restore: an unknown domain waits at its saved directory; a rejected or stale route comes up disabled")
{
    Dirs dirs;
    auto const cfg = loadTestConfig(dirs.config.string());
    auto const legs = nmosroute::enumerateLegs(cfg, kCard);
    auto const& video = legOf(legs, false, nmosroute::LegKind::Video);
    auto const& audio = legOf(legs, false, nmosroute::LegKind::Audio);
    // Only this function's own domain is known: the others are not up yet.
    std::vector<nmosroute::DomainBinding> const domains{{kPrimaryId, dirs.primary.string()}};
    auto const later = dirs.root / "not-created-yet";

    std::vector<nmosroute::SavedActivation> saved(4);
    saved[0].legId = video.id;
    saved[0].masterEnable = true;
    saved[0].domainId = kDomainA;
    saved[0].domainPath = later.string();
    saved[0].flowId = kVideoFlow;
    saved[1].legId = audio.id;
    saved[1].masterEnable = true;
    saved[1].domainId = kDomainB; // no directory saved: cannot be found
    saved[1].flowId = kAudioFlow;
    saved[2].legId = "99999999-9999-4999-8999-999999999999"; // a removed channel
    saved[2].masterEnable = true;
    saved[3].legId = legOf(legs, true, nmosroute::LegKind::Video).id;
    saved[3].sender = true;
    saved[3].masterEnable = true;

    auto channels = cfg.channels;
    auto const outcomes = nmosroute::restoreActivations(channels, legs, saved, domains, kPrimaryId, dirs.primary.string());
    REQUIRE(outcomes.size() == 4);

    using Kind = nmosroute::RestoreOutcome::Kind;
    CHECK(outcomes[0].kind == Kind::Waiting);
    CHECK(channels[1].videoMxlActive);
    CHECK(channels[1].videoFlowId.toString() == kVideoFlow);
    CHECK(channels[1].readerDomainPath == later.string());
    CHECK_FALSE(fs::exists(later)); // never created by the restore

    CHECK(outcomes[1].kind == Kind::Disabled);
    CHECK_FALSE(outcomes[1].active.masterEnable);
    CHECK_FALSE(channels[1].audioFlows[0].mxlActive);
    CHECK(channels[1].audioFlows[0].flowId.toString() == kAudioFlow); // kept for the controller

    CHECK(outcomes[2].kind == Kind::Skipped);

    // A sender restores master_enable with its own flow id.
    CHECK(outcomes[3].kind == Kind::Restored);
    CHECK(outcomes[3].active.flowId == std::optional<std::string>(cfg.channels[0].videoFlowId.toString()));
    CHECK(channels[0].videoMxlActive);
    CHECK(channels[0].videoFlowId == cfg.channels[0].videoFlowId);
}
