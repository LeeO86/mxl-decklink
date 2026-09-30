// SPDX-License-Identifier: MIT
#include "nmos/node.hpp"

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <utility>

#include "cpprest/host_utils.h"

#include "nmos/activation_mode.h"
#include "nmos/capabilities.h"
#include "nmos/channels.h"
#include "nmos/colorspace.h"
#include "nmos/clock_name.h"
#include "nmos/connection_api.h"
#include "nmos/connection_resources.h"
#include "nmos/format.h"
#include "nmos/group_hint.h"
#include "nmos/interlace_mode.h"
#include "nmos/log_gate.h"
#include "nmos/media_type.h"
#include "nmos/model.h"
#include "nmos/mxl.h"
#include "nmos/node_interfaces.h"
#include "nmos/node_resource.h"
#include "nmos/node_resources.h"
#include "nmos/node_server.h"
#include "nmos/resources.h"
#include "nmos/server.h"
#include "nmos/settings.h"
#include "nmos/slog.h"
#include "nmos/transfer_characteristic.h"
#include "nmos/transport.h"
#include "nmos/version.h"
#include "sdp/json.h"

#include "mxlbridge/domainscan.hpp"
#include "nmos/routing.hpp"
#include "util/logging.hpp"
#include "util/uuid.hpp"

namespace mxldl::nmosnode
{
    namespace
    {
        utility::string_t us(std::string const& s)
        {
            return utility::conversions::to_string_t(s);
        }

        std::string su(utility::string_t const& s)
        {
            return utility::conversions::to_utf8string(s);
        }

        nmos::rational rateOf(config::VideoMode const& mode)
        {
            return nmos::rational{mode.rateNumerator, mode.rateDenominator};
        }

        nmos::interlace_mode interlaceOf(config::VideoMode const& mode)
        {
            return mode.interlaced ? nmos::interlace_modes::interlaced_tff : nmos::interlace_modes::progressive;
        }

        nmos::media_type videoMedia(config::ChannelConfig const& ch)
        {
            return ch.pixelFormat == config::PixelFormat::YUVA10 ? nmos::media_types::video_v210a : nmos::media_types::video_v210;
        }

        config::VideoMode nominalMode(config::ChannelConfig const& ch)
        {
            if (ch.videoMode)
            {
                return *ch.videoMode;
            }
            if (auto const hd = config::lookupVideoMode("HD1080p50"))
            {
                return *hd;
            }
            config::VideoMode fallback;
            fallback.name = "HD1080p50";
            fallback.width = 1920;
            fallback.height = 1080;
            fallback.rateNumerator = 50;
            fallback.rateDenominator = 1;
            return fallback;
        }

        void tagGroup(nmos::resource& resource, std::string const& group, std::string const& role)
        {
            if (!resource.data.has_field(nmos::fields::tags))
            {
                resource.data[nmos::fields::tags] = web::json::value::object();
            }
            auto& tags = resource.data[U("tags")];
            if (!tags.is_object())
            {
                tags = web::json::value::object();
            }
            web::json::push_back(tags[U("urn:x-nmos:tag:grouphint/v1.0")], nmos::make_group_hint({us(group), us(role)}));
        }

        void setLabel(nmos::resource& resource, std::string const& label, std::string const& description)
        {
            resource.data[U("label")] = web::json::value::string(us(label));
            resource.data[U("description")] = web::json::value::string(us(description));
        }

        std::vector<nmos::channel> audioChannels(int count)
        {
            std::vector<nmos::channel> out;
            out.reserve(static_cast<std::size_t>(count));
            for (int i = 1; i <= count; ++i)
            {
                out.push_back(nmos::channel{us("Ch" + std::to_string(i)), nmos::channel_symbols::Undefined(static_cast<unsigned>(i))});
            }
            return out;
        }

        web::json::value capsWith(web::json::value set)
        {
            web::json::value sets = web::json::value::array();
            web::json::push_back(sets, std::move(set));
            web::json::value caps = web::json::value::object();
            caps[U("constraint_sets")] = std::move(sets);
            caps[U("version")] = web::json::value::string(nmos::make_version());
            return caps;
        }

        bool legActive(nmosroute::Leg const& leg, config::ChannelConfig const& ch)
        {
            if (leg.kind == nmosroute::LegKind::Video)
            {
                return ch.videoMxlActive && !ch.videoFlowId.isNil();
            }
            if (leg.kind == nmosroute::LegKind::Anc)
            {
                return ch.ancMxlActive && ch.ancFlowId && !ch.ancFlowId->isNil();
            }
            for (auto const& af : ch.audioFlows)
            {
                if (af.index == leg.audioIndex)
                {
                    return af.mxlActive && !af.flowId.isNil();
                }
            }
            return false;
        }
    }

    struct Node::Impl
    {
        config::Config cfg;
        channel::ChannelManager& channels;
        std::uint32_t cardId = 0;
        std::string cardName;

        std::string nodeId;
        std::string deviceId;
        std::string primaryDomainId;
        std::string primaryDomainPath;
        std::vector<nmosroute::DomainBinding> domains;
        std::vector<nmosroute::Leg> legs;

        std::mutex flowMu;
        std::map<std::string, std::string> flowByLeg;

        std::mutex modelMu;
        nmos::node_model* model = nullptr;
        std::atomic<bool> accept{true};

        std::mutex readyMu;
        std::condition_variable readyCv;
        bool ready = false;
        std::string error;

        struct Job
        {
            nmosroute::Activation activation;
        };
        std::mutex jobMu;
        std::condition_variable jobCv;
        std::queue<Job> jobs;
        std::atomic<bool> jobStop{false};
        std::thread jobsThread;
        std::thread serverThread;

        explicit Impl(config::Config const& c, channel::ChannelManager& ch, std::uint32_t id, std::string name)
            : cfg(c)
            , channels(ch)
            , cardId(id)
            , cardName(std::move(name))
        {
        }

        void publishError(std::string message)
        {
            std::lock_guard const lock{readyMu};
            error = std::move(message);
            ready = true;
            readyCv.notify_all();
        }

        void publishReady()
        {
            std::lock_guard const lock{readyMu};
            ready = true;
            readyCv.notify_all();
        }

        config::ChannelConfig const* channelFor(int index) const
        {
            for (auto const& ch : cfg.channels)
            {
                if (ch.index == index)
                {
                    return &ch;
                }
            }
            return nullptr;
        }

        std::string currentFlow(std::string const& legId)
        {
            std::lock_guard const lock{flowMu};
            auto const it = flowByLeg.find(legId);
            return it == flowByLeg.end() ? std::string{} : it->second;
        }

        void rememberFlow(std::string const& legId, std::string const& flowId)
        {
            std::lock_guard const lock{flowMu};
            if (!flowId.empty())
            {
                flowByLeg[legId] = flowId;
            }
        }

        std::string resolveReceiverDomain(std::string const& flowId)
        {
            if (domains.size() == 1)
            {
                return domains.front().id;
            }
            if (!flowId.empty())
            {
                for (auto const& domain : domains)
                {
                    for (auto const& flow : mxlbridge::listFlows(domain.path))
                    {
                        if (flow.id == flowId)
                        {
                            return domain.id;
                        }
                    }
                }
            }
            return {};
        }

        void insert(nmos::node_model& model, nmos::resources& resources, nmos::resource&& resource)
        {
            auto const id = resource.id;
            if (!nmos::insert_resource(resources, std::move(resource)).second)
            {
                throw std::runtime_error("failed to insert NMOS resource " + su(id));
            }
        }

        void buildResources(nmos::node_model& model)
        {
            using web::json::value;
            using web::json::value_of;

            auto const clocks = value_of({nmos::make_internal_clock(nmos::clock_names::clk0)});
            auto const hostInterfaces = nmos::get_host_interfaces(model.settings);
            auto const interfaces = nmos::experimental::node_interfaces(hostInterfaces);

            auto node = nmos::make_node(us(nodeId), clocks, nmos::make_node_interfaces(interfaces), model.settings);
            setLabel(node, cfg.nmosLabel.empty() ? cardName : cfg.nmosLabel, "mxl-decklink BCP-007-03 node");
            insert(model, model.node_resources, std::move(node));

            std::vector<nmos::id> senderIds;
            std::vector<nmos::id> receiverIds;
            for (auto const& leg : legs)
            {
                (leg.sender ? senderIds : receiverIds).push_back(us(leg.id));
            }
            auto device = nmos::make_device(us(deviceId), us(nodeId), senderIds, receiverIds, model.settings);
            setLabel(device, cardName.empty() ? "DeckLink" : cardName, "DeckLink card bridged to MXL");
            insert(model, model.node_resources, std::move(device));

            for (auto const& leg : legs)
            {
                auto const* ch = channelFor(leg.channelIndex);
                if (ch == nullptr)
                {
                    continue;
                }
                auto const mode = nominalMode(*ch);
                auto const active = legActive(leg, *ch);
                auto const group = ch->groupHint.empty() ? ch->label : ch->groupHint;
                rememberFlow(leg.id, leg.initialFlowId);

                if (leg.sender)
                {
                    nmos::resource source;
                    nmos::resource flow;
                    auto const flowId = leg.initialFlowId;
                    if (leg.kind == nmosroute::LegKind::Video)
                    {
                        source = nmos::make_video_source(us(leg.sourceId), us(deviceId), nmos::clock_names::clk0, rateOf(mode), model.settings);
                        flow = nmos::make_coded_video_flow(us(flowId), us(leg.sourceId), us(deviceId), rateOf(mode), mode.width, mode.height, interlaceOf(mode),
                            nmos::colorspaces::BT709, nmos::transfer_characteristics::SDR, sdp::samplings::YCbCr_4_2_2, 10, videoMedia(*ch), model.settings);
                    }
                    else if (leg.kind == nmosroute::LegKind::Audio)
                    {
                        config::AudioFlowConfig const* af = nullptr;
                        for (auto const& candidate : ch->audioFlows)
                        {
                            if (candidate.index == leg.audioIndex)
                            {
                                af = &candidate;
                            }
                        }
                        int const count = af != nullptr ? af->channelCount : 2;
                        source = nmos::make_audio_source(us(leg.sourceId), us(deviceId), nmos::clock_names::clk0, rateOf(mode), audioChannels(count), model.settings);
                        flow = nmos::make_raw_audio_flow(us(flowId), us(leg.sourceId), us(deviceId), nmos::rational{48000, 1}, nmos::media_types::audio_float32, 32,
                            model.settings);
                        flow.data[U("channel_count")] = count;
                    }
                    else
                    {
                        source = nmos::make_data_source(us(leg.sourceId), us(deviceId), nmos::clock_names::clk0, rateOf(mode), model.settings);
                        flow = nmos::make_sdianc_data_flow(us(flowId), us(leg.sourceId), us(deviceId), model.settings);
                        flow.data[U("grain_rate")] = nmos::make_rational(rateOf(mode));
                        flow.data[U("media_type")] = value::string(nmos::media_types::video_smpte291.name);
                    }
                    auto const role = leg.kind == nmosroute::LegKind::Video ? "Video" : leg.kind == nmosroute::LegKind::Audio ? "Audio" : "Data";
                    setLabel(source, ch->label + " " + role, "mxl-decklink source");
                    setLabel(flow, ch->label + " " + role, "mxl-decklink flow");
                    tagGroup(source, group, role);
                    tagGroup(flow, group, role);
                    insert(model, model.node_resources, std::move(source));
                    insert(model, model.node_resources, std::move(flow));

                    auto sender = nmos::make_sender(us(leg.id), us(flowId), nmos::transports::mxl, us(deviceId), {}, {}, model.settings);
                    sender.data[U("subscription")][U("active")] = value::boolean(active);
                    setLabel(sender, ch->label + " " + role, "MXL sender");
                    tagGroup(sender, group, role);
                    insert(model, model.node_resources, std::move(sender));

                    auto connection = nmos::make_connection_mxl_sender(us(leg.id), us(primaryDomainId), {});
                    auto& activeParams = connection.data[U("active")][U("transport_params")][0];
                    auto& stagedParams = connection.data[U("staged")][U("transport_params")][0];
                    activeParams[U("mxl_domain_id")] = value::string(us(primaryDomainId));
                    stagedParams[U("mxl_domain_id")] = value::string(us(primaryDomainId));
                    auto const parsedFlow = util::parseUuid(leg.initialFlowId);
                    if (!parsedFlow || parsedFlow->isNil())
                    {
                        activeParams[U("mxl_flow_id")] = value::null();
                        stagedParams[U("mxl_flow_id")] = value::null();
                    }
                    else
                    {
                        activeParams[U("mxl_flow_id")] = value::string(us(leg.initialFlowId));
                        stagedParams[U("mxl_flow_id")] = value::string(us(leg.initialFlowId));
                    }
                    connection.data[U("active")][U("master_enable")] = value::boolean(active);
                    connection.data[U("staged")][U("master_enable")] = value::boolean(active);
                    connection.data[U("transportfile")] = value::null();
                    insert(model, model.connection_resources, std::move(connection));
                }
                else
                {
                    nmos::resource receiver;
                    if (leg.kind == nmosroute::LegKind::Video)
                    {
                        auto const media = videoMedia(*ch);
                        receiver = nmos::make_receiver(us(leg.id), us(deviceId), nmos::transports::mxl, {}, nmos::formats::video, {media}, model.settings);
                        web::json::value set = web::json::value::object();
                        set[U("urn:x-nmos:cap:format:media_type")] = nmos::make_caps_string_constraint({media.name});
                        set[U("urn:x-nmos:cap:format:grain_rate")] = nmos::make_caps_rational_constraint({rateOf(mode)});
                        set[U("urn:x-nmos:cap:format:frame_width")] = nmos::make_caps_integer_constraint({static_cast<int64_t>(mode.width)});
                        set[U("urn:x-nmos:cap:format:frame_height")] = nmos::make_caps_integer_constraint({static_cast<int64_t>(mode.height)});
                        set[U("urn:x-nmos:cap:format:interlace_mode")] = nmos::make_caps_string_constraint({interlaceOf(mode).name});
                        set[U("urn:x-nmos:cap:format:color_sampling")] = nmos::make_caps_string_constraint({sdp::samplings::YCbCr_4_2_2.name});
                        set[U("urn:x-nmos:cap:format:component_depth")] = nmos::make_caps_integer_constraint({int64_t{10}});
                        receiver.data[U("caps")] = capsWith(std::move(set));
                    }
                    else if (leg.kind == nmosroute::LegKind::Audio)
                    {
                        int count = 2;
                        for (auto const& af : ch->audioFlows)
                        {
                            if (af.index == leg.audioIndex)
                            {
                                count = af.channelCount;
                            }
                        }
                        receiver = nmos::make_receiver(us(leg.id), us(deviceId), nmos::transports::mxl, {}, nmos::formats::audio, {nmos::media_types::audio_float32},
                            model.settings);
                        web::json::value set = web::json::value::object();
                        set[U("urn:x-nmos:cap:format:media_type")] = nmos::make_caps_string_constraint({nmos::media_types::audio_float32.name});
                        set[U("urn:x-nmos:cap:format:channel_count")] = nmos::make_caps_integer_constraint({static_cast<int64_t>(count)});
                        set[U("urn:x-nmos:cap:format:sample_rate")] = nmos::make_caps_rational_constraint({nmos::rational{48000, 1}});
                        set[U("urn:x-nmos:cap:format:sample_depth")] = nmos::make_caps_integer_constraint({int64_t{32}});
                        receiver.data[U("caps")] = capsWith(std::move(set));
                    }
                    else
                    {
                        receiver = nmos::make_sdianc_data_receiver(us(leg.id), us(deviceId), nmos::transports::mxl, {}, model.settings);
                        web::json::value set = web::json::value::object();
                        set[U("urn:x-nmos:cap:format:media_type")] = nmos::make_caps_string_constraint({nmos::media_types::video_smpte291.name});
                        set[U("urn:x-nmos:cap:format:grain_rate")] = nmos::make_caps_rational_constraint({rateOf(mode)});
                        receiver.data[U("caps")] = capsWith(std::move(set));
                    }
                    auto const role = leg.kind == nmosroute::LegKind::Video ? "Video" : leg.kind == nmosroute::LegKind::Audio ? "Audio" : "Data";
                    setLabel(receiver, ch->label + " " + role, "MXL receiver");
                    tagGroup(receiver, group, role);
                    receiver.data[U("version")] = value::string(nmos::make_version());
                    insert(model, model.node_resources, std::move(receiver));

                    web::json::value domainEnum = web::json::value::array();
                    for (auto const& domain : domains)
                    {
                        web::json::push_back(domainEnum, value::string(us(domain.id)));
                    }
                    auto connection = nmos::make_connection_mxl_receiver(us(leg.id), domains.size() == 1 ? us(domains.front().id) : utility::string_t{});
                    if (domains.size() != 1)
                    {
                        web::json::value domainConstraint = web::json::value::object();
                        domainConstraint[U("enum")] = domainEnum;
                        connection.data[U("constraints")][0][U("mxl_domain_id")] = std::move(domainConstraint);
                    }
                    auto& activeParams = connection.data[U("active")][U("transport_params")][0];
                    auto& stagedParams = connection.data[U("staged")][U("transport_params")][0];
                    activeParams[U("mxl_domain_id")] = value::string(us(primaryDomainId));
                    stagedParams[U("mxl_domain_id")] = value::string(us(primaryDomainId));
                    if (!active)
                    {
                        activeParams[U("mxl_flow_id")] = value::null();
                        stagedParams[U("mxl_flow_id")] = value::null();
                    }
                    else
                    {
                        activeParams[U("mxl_flow_id")] = value::string(us(leg.initialFlowId));
                        stagedParams[U("mxl_flow_id")] = value::string(us(leg.initialFlowId));
                    }
                    connection.data[U("active")][U("master_enable")] = value::boolean(active);
                    connection.data[U("staged")][U("master_enable")] = value::boolean(active);
                    insert(model, model.connection_resources, std::move(connection));
                }
            }
            model.notify();
        }

        void onRuntimeFlows(int channelIndex, channel::InputChannel::RuntimeFlows const& flows)
        {
            if (!accept.load())
            {
                return;
            }
            std::lock_guard const guard{modelMu};
            if (model == nullptr)
            {
                return;
            }
            auto lock = model->write_lock();
            for (auto const& leg : legs)
            {
                if (!leg.sender || leg.channelIndex != channelIndex)
                {
                    continue;
                }
                std::string flowId;
                config::VideoMode mode = flows.mode;
                if (leg.kind == nmosroute::LegKind::Video)
                {
                    flowId = flows.videoId;
                }
                else if (leg.kind == nmosroute::LegKind::Anc)
                {
                    if (!flows.ancId)
                    {
                        continue;
                    }
                    flowId = *flows.ancId;
                }
                else
                {
                    for (auto const& audio : flows.audioIds)
                    {
                        if (audio.first == leg.audioIndex)
                        {
                            flowId = audio.second;
                        }
                    }
                }
                if (flowId.empty())
                {
                    continue;
                }
                rememberFlow(leg.id, flowId);
                auto const* ch = channelFor(leg.channelIndex);
                if (ch == nullptr)
                {
                    continue;
                }
                auto const version = web::json::value::string(nmos::make_version());
                nmos::id oldFlow;
                if (auto const sender = nmos::find_resource(model->node_resources, {us(leg.id), nmos::types::sender});
                    sender != model->node_resources.end() && sender->data.has_field(U("flow_id")) && sender->data.at(U("flow_id")).is_string())
                {
                    oldFlow = sender->data.at(U("flow_id")).as_string();
                }
                if (oldFlow != us(flowId))
                {
                    nmos::resource flow;
                    if (leg.kind == nmosroute::LegKind::Video)
                    {
                        flow = nmos::make_coded_video_flow(us(flowId), us(leg.sourceId), us(deviceId), rateOf(mode), mode.width, mode.height, interlaceOf(mode),
                            nmos::colorspaces::BT709, nmos::transfer_characteristics::SDR, sdp::samplings::YCbCr_4_2_2, 10, videoMedia(*ch), model->settings);
                        setLabel(flow, ch->label + " Video", "mxl-decklink flow");
                    }
                    else if (leg.kind == nmosroute::LegKind::Audio)
                    {
                        int count = 2;
                        for (auto const& af : ch->audioFlows)
                        {
                            if (af.index == leg.audioIndex)
                            {
                                count = af.channelCount;
                            }
                        }
                        flow = nmos::make_raw_audio_flow(us(flowId), us(leg.sourceId), us(deviceId), nmos::rational{48000, 1}, nmos::media_types::audio_float32, 32,
                            model->settings);
                        flow.data[U("channel_count")] = count;
                        setLabel(flow, ch->label + " Audio", "mxl-decklink flow");
                    }
                    else
                    {
                        flow = nmos::make_sdianc_data_flow(us(flowId), us(leg.sourceId), us(deviceId), model->settings);
                        flow.data[U("grain_rate")] = nmos::make_rational(rateOf(mode));
                        setLabel(flow, ch->label + " Data", "mxl-decklink flow");
                    }
                    insert(*model, model->node_resources, std::move(flow));
                    if (!oldFlow.empty())
                    {
                        nmos::erase_resource(model->node_resources, oldFlow);
                    }
                }
                nmos::modify_resource(model->node_resources, us(leg.id), [&](nmos::resource& resource) {
                    if (resource.data.has_field(U("flow_id")))
                    {
                        resource.data[U("flow_id")] = web::json::value::string(us(flowId));
                    }
                    resource.data[U("version")] = version;
                });
                nmos::modify_resource(model->connection_resources, us(leg.id), [&](nmos::resource& resource) {
                    resource.data[U("active")][U("transport_params")][0][U("mxl_flow_id")] = web::json::value::string(us(flowId));
                    resource.data[U("version")] = version;
                });
            }
            model->notify();
        }

        void workerLoop()
        {
            while (!jobStop.load())
            {
                Job job;
                {
                    std::unique_lock lock{jobMu};
                    jobCv.wait(lock, [&] { return jobStop.load() || !jobs.empty(); });
                    if (jobStop.load() && jobs.empty())
                    {
                        return;
                    }
                    job = std::move(jobs.front());
                    jobs.pop();
                }
                auto cfgs = channels.channelConfigs();
                if (auto const err = nmosroute::applyActivation(cfgs, legs, job.activation, domains, primaryDomainPath))
                {
                    log::error("nmos_activation_rejected", {{"leg", job.activation.legId}, {"details", *err}});
                    continue;
                }
                if (job.activation.flowId)
                {
                    rememberFlow(job.activation.legId, *job.activation.flowId);
                }
                auto const applied = channels.applyChannels(cfgs);
                if (std::holds_alternative<std::string>(applied))
                {
                    log::error("nmos_activation_apply_failed", {{"leg", job.activation.legId}, {"details", std::get<std::string>(applied)}});
                }
                else
                {
                    log::info("nmos_activation_applied",
                        {
                            {"leg", job.activation.legId},
                            {"master_enable", job.activation.masterEnable},
                            {"domain_id", job.activation.domainId},
                            {"flow_id", job.activation.flowId.value_or("")},
                        });
                }
            }
        }

        void enqueue(nmosroute::Activation activation)
        {
            {
                std::lock_guard const lock{jobMu};
                jobs.push(Job{std::move(activation)});
            }
            jobCv.notify_one();
        }

        nmosroute::Activation readActivation(nmos::resource const& connection)
        {
            nmosroute::Activation activation;
            activation.legId = su(connection.id);
            auto const& active = connection.data.at(U("active"));
            activation.masterEnable = active.at(U("master_enable")).as_bool();
            auto const& params = active.at(U("transport_params")).at(0);
            if (params.has_field(U("mxl_domain_id")) && params.at(U("mxl_domain_id")).is_string())
            {
                activation.domainId = su(params.at(U("mxl_domain_id")).as_string());
            }
            if (params.has_field(U("mxl_flow_id")) && params.at(U("mxl_flow_id")).is_string())
            {
                auto const flow = su(params.at(U("mxl_flow_id")).as_string());
                if (flow != "auto")
                {
                    activation.flowId = flow;
                }
            }
            return activation;
        }

        void serverMain()
        {
            nmos::experimental::log_model logModel;
            std::ostream errorLog(std::cerr.rdbuf());
            std::filebuf discarded;
            std::ostream accessLog(&discarded);
            nmos::experimental::log_gate gate(errorLog, accessLog, logModel);

            try
            {
                nmos::node_model nodeModel;
                nodeId = nmosroute::nodeIdForCard(cardId);
                deviceId = nmosroute::stableId(cardId, "device");
                web::json::value settings = web::json::value::object();
                settings[U("http_port")] = cfg.nmosPort;
                settings[U("label")] = web::json::value::string(us(cfg.nmosLabel.empty() ? cardName : cfg.nmosLabel));
                settings[U("description")] = web::json::value::string(U("mxl-decklink"));
                settings[U("seed_id")] = web::json::value::string(us(nodeId));
                settings[U("service_name_prefix")] = web::json::value::string(U("mxl-decklink"));
                settings[U("logging_level")] = 20;
                settings[U("control_protocol_ws_port")] = -1;
                if (!cfg.nmosRegistryAddress.empty())
                {
                    settings[U("registry_address")] = web::json::value::string(us(cfg.nmosRegistryAddress));
                    settings[U("registration_port")] = cfg.nmosRegistryPort;
                }
                nodeModel.settings = settings;
                nmos::insert_node_default_settings(nodeModel.settings);
                logModel.settings = nodeModel.settings;
                logModel.level = nmos::fields::logging_level(logModel.settings);

                primaryDomainPath = cfg.domainPath;
                primaryDomainId = mxlbridge::ensureDomainId(cfg.domainPath);
                domains.clear();
                bool sawPrimary = false;
                for (auto const& found : mxlbridge::scanDomains(cfg.domainScanPath))
                {
                    if (!found.id)
                    {
                        continue;
                    }
                    domains.push_back(nmosroute::DomainBinding{*found.id, found.path});
                    if (found.path == primaryDomainPath || (found.id && *found.id == primaryDomainId))
                    {
                        sawPrimary = true;
                    }
                }
                if (!sawPrimary)
                {
                    domains.push_back(nmosroute::DomainBinding{primaryDomainId, primaryDomainPath});
                }
                legs = nmosroute::enumerateLegs(cfg, cardId);

                // Park unassigned output receivers so they wait for IS-05 instead of retrying.
                {
                    auto cfgs = channels.channelConfigs();
                    bool changed = false;
                    for (auto& ch : cfgs)
                    {
                        if (ch.direction == config::Direction::Output && ch.videoFlowId.isNil() && ch.videoMxlActive)
                        {
                            ch.videoMxlActive = false;
                            changed = true;
                        }
                    }
                    if (changed)
                    {
                        auto const applied = channels.applyChannels(cfgs);
                        if (auto const err = std::get_if<std::string>(&applied))
                        {
                            throw std::runtime_error(*err);
                        }
                        cfg.channels = std::move(cfgs);
                    }
                }

                auto implementation = nmos::experimental::node_implementation()
                                          .on_parse_transport_file([](nmos::resource const&, nmos::resource const&, utility::string_t const&, utility::string_t const&,
                                                                       slog::base_gate&) -> web::json::value {
                                              throw std::runtime_error("MXL does not use a transport file");
                                          })
                                          .on_resolve_auto([this](nmos::resource const& resource, nmos::resource const&, web::json::value& params) {
                                              if (!params.is_array() || params.size() == 0)
                                              {
                                                  return;
                                              }
                                              auto& legParams = params.at(0);
                                              bool const sender = resource.type == nmos::types::sender;
                                              nmos::details::resolve_auto(legParams, U("mxl_domain_id"), [&] {
                                                  if (sender)
                                                  {
                                                      return web::json::value::string(us(primaryDomainId));
                                                  }
                                                  std::string flow;
                                                  if (legParams.has_field(U("mxl_flow_id")) && legParams.at(U("mxl_flow_id")).is_string())
                                                  {
                                                      auto const text = su(legParams.at(U("mxl_flow_id")).as_string());
                                                      if (text != "auto")
                                                      {
                                                          flow = text;
                                                      }
                                                  }
                                                  auto const domain = resolveReceiverDomain(flow);
                                                  if (domain.empty())
                                                  {
                                                      throw std::runtime_error("cannot resolve mxl_domain_id");
                                                  }
                                                  return web::json::value::string(us(domain));
                                              });
                                              if (sender)
                                              {
                                                  nmos::details::resolve_auto(legParams, U("mxl_flow_id"), [&] {
                                                      auto const flow = currentFlow(su(resource.id));
                                                      if (flow.empty())
                                                      {
                                                          throw std::runtime_error("cannot resolve mxl_flow_id");
                                                      }
                                                      return web::json::value::string(us(flow));
                                                  });
                                              }
                                          })
                                          .on_set_transportfile([](nmos::resource const&, nmos::resource const&, web::json::value& transportFile) {
                                              transportFile = web::json::value::null();
                                          })
                                          .on_connection_activated([this](nmos::resource const&, nmos::resource const& connection) {
                                              if (!accept.load())
                                              {
                                                  return;
                                              }
                                              enqueue(readActivation(connection));
                                          });

                auto server = nmos::experimental::make_node_server(nodeModel, implementation, logModel, gate);
                server.thread_functions.push_back([this, &nodeModel] {
                    try
                    {
                        {
                            auto lock = nodeModel.write_lock();
                            buildResources(nodeModel);
                        }
                        publishReady();
                        auto lock = nodeModel.write_lock();
                        nodeModel.wait(lock, [&] { return nodeModel.shutdown; });
                    }
                    catch (std::exception const& ex)
                    {
                        publishError(ex.what());
                    }
                });

                {
                    std::lock_guard const guard{modelMu};
                    model = &nodeModel;
                }
                nmos::server_guard guard(server);
                log::info("nmos_node_ready",
                    {
                        {"port", cfg.nmosPort},
                        {"node_id", nodeId},
                        {"domain_id", primaryDomainId},
                        {"registry", cfg.nmosRegistryAddress.empty() ? "dns-sd" : cfg.nmosRegistryAddress},
                    });

                {
                    std::unique_lock lock{readyMu};
                    readyCv.wait(lock, [&] { return ready; });
                }
                if (!error.empty())
                {
                    throw std::runtime_error(error);
                }

                // Block until stop() flags shutdown. The implementation thread
                // observes nodeModel.shutdown; this thread just keeps the guard alive.
                while (accept.load())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                {
                    auto lock = nodeModel.write_lock();
                    nodeModel.shutdown = true;
                    nodeModel.notify();
                }
                {
                    std::lock_guard const guard{modelMu};
                    model = nullptr;
                }
            }
            catch (std::exception const& ex)
            {
                publishError(ex.what());
                log::error("nmos_node_failed", {{"details", ex.what()}});
                std::lock_guard const guard{modelMu};
                model = nullptr;
            }
        }
    };

    Node::Node(config::Config const& cfg, channel::ChannelManager& channels, std::uint32_t cardPersistentId, std::string cardName)
        : _impl(std::make_unique<Impl>(cfg, channels, cardPersistentId, std::move(cardName)))
    {
    }

    Node::~Node()
    {
        stop();
    }

    void Node::start()
    {
        _impl->channels.setRuntimeFlowsHandler([impl = _impl.get()](int index, channel::InputChannel::RuntimeFlows const& flows) {
            impl->onRuntimeFlows(index, flows);
        });
        _impl->jobsThread = std::thread([impl = _impl.get()] { impl->workerLoop(); });
        _impl->serverThread = std::thread([impl = _impl.get()] { impl->serverMain(); });

        std::unique_lock lock{_impl->readyMu};
        _impl->readyCv.wait(lock, [&] { return _impl->ready; });
        if (!_impl->error.empty())
        {
            auto const message = _impl->error;
            lock.unlock();
            stop();
            throw std::runtime_error(message);
        }
    }

    void Node::stop()
    {
        if (!_impl)
        {
            return;
        }
        _impl->accept.store(false);
        _impl->jobStop.store(true);
        _impl->jobCv.notify_all();
        if (_impl->jobsThread.joinable())
        {
            _impl->jobsThread.join();
        }
        if (_impl->serverThread.joinable())
        {
            _impl->serverThread.join();
        }
    }
}
