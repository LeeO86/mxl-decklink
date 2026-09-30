// SPDX-License-Identifier: MIT
#include "nmos/routing.hpp"

#include <filesystem>

#include "util/uuid.hpp"

namespace mxldl::nmosroute
{
    namespace
    {
        util::Uuid cardSeed(std::uint32_t cardPersistentId)
        {
            util::Uuid seed{};
            seed.bytes[0] = static_cast<std::uint8_t>(cardPersistentId >> 24);
            seed.bytes[1] = static_cast<std::uint8_t>(cardPersistentId >> 16);
            seed.bytes[2] = static_cast<std::uint8_t>(cardPersistentId >> 8);
            seed.bytes[3] = static_cast<std::uint8_t>(cardPersistentId);
            seed.bytes[6] = 0x40;
            seed.bytes[8] = 0x80;
            return seed;
        }

        std::string idFor(util::Uuid const& seed, std::string const& name)
        {
            return util::deriveUuid(seed, name).toString();
        }

        config::ChannelConfig* findChannel(std::vector<config::ChannelConfig>& channels, int index)
        {
            for (auto& ch : channels)
            {
                if (ch.index == index)
                {
                    return &ch;
                }
            }
            return nullptr;
        }

        DomainBinding const* findDomain(std::vector<DomainBinding> const& domains, std::string const& id)
        {
            for (auto const& d : domains)
            {
                if (d.id == id)
                {
                    return &d;
                }
            }
            return nullptr;
        }
    }

    std::string stableId(std::uint32_t cardPersistentId, std::string const& name)
    {
        return idFor(cardSeed(cardPersistentId), name);
    }

    std::string nodeIdForCard(std::uint32_t cardPersistentId)
    {
        return stableId(cardPersistentId, "node");
    }

    std::vector<Leg> enumerateLegs(config::Config const& cfg, std::uint32_t cardPersistentId)
    {
        auto const seed = cardSeed(cardPersistentId);
        std::vector<Leg> legs;
        for (auto const& ch : cfg.channels)
        {
            bool const sender = ch.direction == config::Direction::Input;
            auto const role = sender ? std::string("sender") : std::string("receiver");
            auto const prefix = "ch" + std::to_string(ch.index) + "/";

            Leg video;
            video.id = idFor(seed, role + "/" + prefix + "video");
            video.channelIndex = ch.index;
            video.kind = LegKind::Video;
            video.sender = sender;
            video.sourceId = idFor(seed, "source/" + prefix + "video");
            video.initialFlowId = ch.videoFlowId.toString();
            legs.push_back(std::move(video));

            if (ch.audioEnable)
            {
                for (auto const& af : ch.audioFlows)
                {
                    auto const key = prefix + "audio" + std::to_string(af.index);
                    Leg audio;
                    audio.id = idFor(seed, role + "/" + key);
                    audio.channelIndex = ch.index;
                    audio.kind = LegKind::Audio;
                    audio.audioIndex = af.index;
                    audio.sender = sender;
                    audio.sourceId = idFor(seed, "source/" + key);
                    audio.initialFlowId = af.flowId.toString();
                    legs.push_back(std::move(audio));
                }
            }

            if (ch.ancEnable && ch.ancFlowId)
            {
                Leg anc;
                anc.id = idFor(seed, role + "/" + prefix + "anc");
                anc.channelIndex = ch.index;
                anc.kind = LegKind::Anc;
                anc.sender = sender;
                anc.sourceId = idFor(seed, "source/" + prefix + "anc");
                anc.initialFlowId = ch.ancFlowId->toString();
                legs.push_back(std::move(anc));
            }
        }
        return legs;
    }

    std::optional<std::string> applyActivation(std::vector<config::ChannelConfig>& channels, std::vector<Leg> const& legs, Activation const& activation,
        std::vector<DomainBinding> const& domains, std::string const& primaryDomainPath)
    {
        Leg const* leg = nullptr;
        for (auto const& candidate : legs)
        {
            if (candidate.id == activation.legId)
            {
                leg = &candidate;
                break;
            }
        }
        if (leg == nullptr)
        {
            return "no NMOS sender/receiver with id " + activation.legId;
        }

        auto* channel = findChannel(channels, leg->channelIndex);
        if (channel == nullptr)
        {
            return "channel " + std::to_string(leg->channelIndex) + " is no longer configured";
        }

        auto const* domain = findDomain(domains, activation.domainId);
        if (activation.masterEnable && domain == nullptr)
        {
            return "mxl_domain_id " + activation.domainId + " is not a domain mounted in this process";
        }
        if (leg->sender && domain != nullptr && domain->path != primaryDomainPath)
        {
            return "MXL senders write only the configured domain " + primaryDomainPath;
        }

        util::Uuid flowId{};
        bool haveFlow = false;
        if (activation.flowId)
        {
            auto const parsed = util::parseUuid(*activation.flowId);
            if (!parsed)
            {
                return "mxl_flow_id is not a UUID";
            }
            flowId = *parsed;
            haveFlow = true;
        }
        if (activation.masterEnable && (!haveFlow || flowId.isNil()))
        {
            return "master_enable requires a non-nil mxl_flow_id";
        }

        auto updated = *channel;
        if (leg->kind == LegKind::Video)
        {
            updated.videoMxlActive = activation.masterEnable;
            if (haveFlow)
            {
                updated.videoFlowId = flowId;
            }
        }
        else if (leg->kind == LegKind::Audio)
        {
            bool found = false;
            for (auto& af : updated.audioFlows)
            {
                if (af.index == leg->audioIndex)
                {
                    af.mxlActive = activation.masterEnable;
                    if (haveFlow)
                    {
                        af.flowId = flowId;
                    }
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                return "audio flow " + std::to_string(leg->audioIndex) + " is not configured on channel " + std::to_string(leg->channelIndex);
            }
        }
        else
        {
            updated.ancMxlActive = activation.masterEnable;
            if (haveFlow)
            {
                updated.ancFlowId = flowId;
            }
        }

        if (!leg->sender)
        {
            if (domain != nullptr && domain->path != primaryDomainPath)
            {
                updated.readerDomainPath = domain->path;
            }
            else
            {
                updated.readerDomainPath.clear();
            }
            if (domain != nullptr && !std::filesystem::is_directory(domain->path))
            {
                return "MXL domain directory does not exist: " + domain->path;
            }
        }

        *channel = std::move(updated);
        return std::nullopt;
    }
}
