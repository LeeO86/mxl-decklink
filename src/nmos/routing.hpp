// SPDX-License-Identifier: MIT
// Maps DeckLink channels onto BCP-007-03 legs and applies one IS-05 activation
// to the in-memory channel configuration. No NMOS library types here so unit
// tests and the default build can use it without nmos-cpp.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "config/config.hpp"

namespace mxldl::nmosroute
{
    enum class LegKind
    {
        Video,
        Audio,
        Anc,
    };

    /// One IS-04 Sender (input / MXL writer) or Receiver (output / MXL reader).
    struct Leg
    {
        std::string id; // stable IS-04/IS-05 resource id
        int channelIndex = 0;
        LegKind kind = LegKind::Video;
        int audioIndex = -1; // index in ChannelConfig::audioFlows
        bool sender = true;
        std::string sourceId; // IS-04 Source (senders only)
        std::string initialFlowId; // configured MXL flow id, possibly nil
    };

    struct DomainBinding
    {
        std::string id;
        std::string path;
    };

    struct Activation
    {
        std::string legId;
        bool masterEnable = false;
        std::string domainId; // resolved UUID, never "auto"
        std::optional<std::string> flowId; // nullopt when the parameter is null
    };

    /// Stable UUID derived from the card persistent id and a role name
    /// ("node", "device", "sender/ch0/video", ...). Used when NMOS_SEED is unset.
    [[nodiscard]] std::string stableId(std::uint32_t cardPersistentId, std::string const& name);

    /// UUIDv5 of `mxl-decklink/<seed>/<name>` under the DNS namespace.
    [[nodiscard]] std::string idFromSeed(std::string const& seed, std::string const& name);

    /// Node id: NMOS_SEED when set, otherwise the card persistent id.
    [[nodiscard]] std::string nodeIdFor(config::Config const& cfg, std::uint32_t cardPersistentId);

    /// Stable node UUID derived from the card persistent id.
    [[nodiscard]] std::string nodeIdForCard(std::uint32_t cardPersistentId);

    /// One leg per video / audio-flow / ANC endpoint on each configured channel.
    [[nodiscard]] std::vector<Leg> enumerateLegs(config::Config const& cfg, std::uint32_t cardPersistentId);

    /// Applies one activation to `channels`. On success the matching channel's
    /// flow id, master-enable flag, and (for receivers) reader domain path are
    /// updated. Returns an error string when the leg, domain, or flow id is
    /// not usable; `channels` is unchanged in that case.
    [[nodiscard]] std::optional<std::string> applyActivation(std::vector<config::ChannelConfig>& channels, std::vector<Leg> const& legs,
        Activation const& activation, std::vector<DomainBinding> const& domains, std::string const& primaryDomainPath);
}
