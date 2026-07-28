// SPDX-License-Identifier: MIT
#include "webapi.hpp"

#include <filesystem>
#include <sstream>
#include <system_error>
#include <unordered_map>

#include <picojson/picojson.h>

#include <mxl/mxl.h>

#include "mxlbridge/domainscan.hpp"
#include "ops/webui_generated.hpp"
#include "util/logging.hpp"
#include "util/taiclock.hpp"
#include "version.hpp"

namespace mxldl::ops
{
    namespace
    {
        std::string jstr(std::string const& s)
        {
            return "\"" + log::jsonEscape(s) + "\"";
        }

        std::string jopt(std::optional<std::string> const& s)
        {
            return s ? jstr(*s) : "null";
        }

        template<typename T>
        std::string jnum(std::optional<T> const& v)
        {
            return v ? std::to_string(*v) : "null";
        }

        HttpResponse jsonResponse(int status, std::string body)
        {
            return {status, "application/json", std::move(body)};
        }

        HttpResponse jsonError(int status, std::string const& message)
        {
            return jsonResponse(status, "{\"error\":" + jstr(message) + "}");
        }

        char const* audioSampleTypeName(config::AudioSampleType t)
        {
            return t == config::AudioSampleType::Int16 ? "16bit" : "32bit";
        }

        /// Appends the channel `audio` object: config matrix + DeckLink buffer
        /// (outputs) + optional MXL domain runtime (active / head_index).
        void appendAudioJson(std::ostringstream& out, channel::ChannelManager::ChannelView const& v,
            std::unordered_map<std::string, mxlbridge::FlowSummary> const* flowById)
        {
            out << ",\"audio\":{";
            out << "\"enabled\":" << (v.cfg.audioEnable ? "true" : "false");
            out << ",\"deck_channel_count\":" << v.cfg.audioChannelCount;
            out << ",\"sample_type\":" << jstr(audioSampleTypeName(v.cfg.audioSampleType));
            if (v.cfg.direction == config::Direction::Output)
            {
                out << ",\"decklink_buffered_audio_frames\":" << v.bufferedAudioFrames;
                out << ",\"decklink_buffered_video_frames\":" << v.bufferedVideoFrames;
            }
            else
            {
                out << ",\"decklink_buffered_audio_frames\":null";
                out << ",\"decklink_buffered_video_frames\":null";
            }
            out << ",\"flows\":[";
            bool firstFlow = true;
            for (auto const& af : v.cfg.audioFlows)
            {
                if (!firstFlow)
                {
                    out << ",";
                }
                firstFlow = false;
                bool const unassigned = af.flowId.isNil();
                out << "{\"index\":" << af.index;
                out << ",\"flow_id\":" << jstr(af.flowId.toString());
                out << ",\"channel_count\":" << af.channelCount;
                out << ",\"label\":" << jstr(af.label);
                out << ",\"unassigned\":" << (unassigned ? "true" : "false");
                out << ",\"map\":[";
                for (std::size_t i = 0; i < af.deckLinkChannels.size(); ++i)
                {
                    if (i > 0)
                    {
                        out << ",";
                    }
                    out << af.deckLinkChannels[i];
                }
                out << "]";
                if (!unassigned && flowById != nullptr)
                {
                    auto const it = flowById->find(af.flowId.toString());
                    if (it != flowById->end())
                    {
                        out << ",\"mxl_present\":true";
                        out << ",\"mxl_active\":" << (it->second.active ? "true" : "false");
                        out << ",\"mxl_head_index\":" << jnum(it->second.headIndex);
                        out << ",\"mxl_last_write_time_ns\":" << jnum(it->second.lastWriteTimeNs);
                        if (it->second.channelCount)
                        {
                            out << ",\"mxl_channel_count\":" << *it->second.channelCount;
                        }
                        else
                        {
                            out << ",\"mxl_channel_count\":null";
                        }
                    }
                    else
                    {
                        out << ",\"mxl_present\":false,\"mxl_active\":false,\"mxl_head_index\":null,\"mxl_last_write_time_ns\":null,\"mxl_channel_count\":null";
                    }
                }
                else
                {
                    out << ",\"mxl_present\":false,\"mxl_active\":false,\"mxl_head_index\":null,\"mxl_last_write_time_ns\":null,\"mxl_channel_count\":null";
                }
                out << "}";
            }
            out << "]}";
        }
    }

    WebService::WebService(config::Config const& activeCfg, config::ConfigStore& store, channel::ChannelManager& channels, dl::ICard& card,
        mxlbridge::Domain& domain, HealthService& health)
        : _activeCfg(activeCfg)
        , _store(store)
        , _channels(channels)
        , _card(card)
        , _domain(domain)
        , _health(health)
        , _startedAtTaiNs(util::taiNowNs())
    {}

    void WebService::start()
    {
        _server = std::make_unique<HttpServer>(
            _activeCfg.webPort,
            [this](HttpRequest const& req) {
                return handle(req);
            },
            "http-web");
        _server->start();
    }

    void WebService::stop()
    {
        if (_server)
        {
            _server->stop();
        }
    }

    HttpResponse WebService::handle(HttpRequest const& req)
    {
        try
        {
            // Always served (§7.1): probes and metrics.
            if (req.path == "/livez")
            {
                return _health.livez();
            }
            if (req.path == "/readyz")
            {
                return _health.readyz();
            }
            if (req.path == "/statusz")
            {
                return _health.statusz();
            }
            if (req.path == "/metrics")
            {
                return _health.metricsText();
            }

            // UI + API only with WEB_ENABLE=true (§7.5.5).
            if (!_activeCfg.webEnable)
            {
                return jsonError(404, "web interface disabled (WEB_ENABLE=false); available endpoints: /livez /readyz /statusz /metrics");
            }
            if (req.path == "/" || req.path == "/index.html")
            {
                return {200, "text/html; charset=utf-8", std::string(webui::indexHtml())};
            }
            if (req.path == "/api/status" && req.method == "GET")
            {
                return apiStatus();
            }
            if (req.path == "/api/card" && req.method == "GET")
            {
                return apiCard();
            }
            if (req.path == "/api/config" && req.method == "GET")
            {
                return apiConfigGet();
            }
            if (req.path == "/api/config" && req.method == "PUT")
            {
                return apiConfigPut(req);
            }
            if (req.path == "/api/domains" && req.method == "GET")
            {
                return apiDomainsGet();
            }
            if (req.path == "/api/domains" && req.method == "POST")
            {
                return apiDomainsPost(req);
            }
            if (req.path == "/api/flows" && req.method == "GET")
            {
                return apiFlowsGet(req);
            }
            return jsonError(404, "not found");
        }
        catch (std::exception const& e)
        {
            log::error("webapi_exception", {{"path", req.path}, {"details", e.what()}});
            return jsonError(500, e.what());
        }
    }

    HttpResponse WebService::apiStatus()
    {
        mxlVersionType mxlVersion{};
        ::mxlGetVersion(&mxlVersion);

        std::ostringstream out;
        out << "{";
        out << "\"version\":" << jstr(kVersion);
        out << ",\"mxl_version\":" << jstr(mxlVersion.full != nullptr ? mxlVersion.full : "?");
        auto const dlVersion = dl::deckLinkApiVersion();
        out << ",\"decklink_api_version\":" << (dlVersion.empty() ? "null" : jstr(dlVersion));
        out << ",\"backend\":" << jstr(_activeCfg.backend);
        out << ",\"card_open_fallback\":" << (_activeCfg.cardOpenFallback ? "true" : "false");
        out << ",\"uptime_s\":" << (util::taiNowNs() - _startedAtTaiNs) / 1'000'000'000ULL;
        out << ",\"restart_required\":" << (_restartRequired.load() ? "true" : "false");

        char cardId[16];
        ::snprintf(cardId, sizeof(cardId), "0x%08x", _card.persistentId());
        out << ",\"card\":{\"name\":" << jstr(_card.displayName()) << ",\"persistent_id\":" << jstr(cardId)
            << ",\"subdevices\":" << _card.subDeviceCount() << "}";

        bool domainTmpfs = false;
        ::mxlIsTmpFs(_domain.path().c_str(), &domainTmpfs);
        out << ",\"domain\":{\"path\":" << jstr(_domain.path()) << ",\"tmpfs\":" << (domainTmpfs ? "true" : "false") << "}";

        out << ",\"channels\":[";
        bool first = true;
        std::unordered_map<std::string, mxlbridge::FlowSummary> flowById;
        try
        {
            for (auto const& f : mxlbridge::listFlows(_domain.path()))
            {
                flowById.emplace(f.id, f);
            }
        }
        catch (...)
        {
            // Domain may be empty / inaccessible during bring-up; omit MXL join.
        }
        for (auto const& v : _channels.channels())
        {
            if (!first)
            {
                out << ",";
            }
            first = false;
            out << "{\"index\":" << v.cfg.index << ",\"label\":" << jstr(v.cfg.label)
                << ",\"direction\":" << jstr(config::directionName(v.cfg.direction)) << ",\"subdevice_index\":" << v.cfg.subdeviceIndex
                << ",\"state\":" << jstr(channel::stateName(v.state)) << ",\"signal_locked\":" << (v.signalLocked ? "true" : "false")
                << ",\"active_video_mode\":" << jstr(v.activeModeName) << ",\"video_mode_setting\":" << jstr(v.cfg.videoModeName)
                << ",\"active_video_flow_id\":" << jstr(v.activeVideoFlowId) << ",\"frames_total\":" << v.framesTotal
                << ",\"frames_dropped\":" << v.framesDropped << ",\"reconnects\":" << v.reconnects << ",\"grains_committed\":" << v.grainsCommitted
                << ",\"last_frame_tai_ns\":" << v.lastFrameTaiNs;
            appendAudioJson(out, v, &flowById);
            out << "}";
        }
        out << "]}";
        return jsonResponse(200, out.str());
    }

    HttpResponse WebService::apiCard()
    {
        std::ostringstream out;
        char cardId[16];
        ::snprintf(cardId, sizeof(cardId), "0x%08x", _card.persistentId());
        out << "{\"name\":" << jstr(_card.displayName()) << ",\"persistent_id\":" << jstr(cardId) << ",\"subdevices\":[";
        for (std::size_t i = 0; i < _card.subDeviceCount(); ++i)
        {
            auto& sub = _card.subDevice(i);
            auto const& info = sub.info();
            auto const status = sub.status();
            if (i > 0)
            {
                out << ",";
            }
            out << "{\"index\":" << i << ",\"display_name\":" << jstr(info.displayName) << ",\"model\":" << jstr(info.modelName)
                << ",\"supports_capture\":" << (info.supportsCapture ? "true" : "false")
                << ",\"supports_playback\":" << (info.supportsPlayback ? "true" : "false")
                << ",\"supports_format_detection\":" << (info.supportsInputFormatDetection ? "true" : "false")
                << ",\"has_profile_manager\":" << (info.hasProfileManager ? "true" : "false") << ",\"status\":{"
                << "\"detected_input_mode\":" << jopt(status.detectedInputMode) << ",\"current_input_mode\":" << jopt(status.currentInputMode)
                << ",\"current_output_mode\":" << jopt(status.currentOutputMode)
                << ",\"current_input_pixel_format\":" << jopt(status.currentInputPixelFormat)
                << ",\"last_output_pixel_format\":" << jopt(status.lastOutputPixelFormat)
                << ",\"input_signal_locked\":" << (status.inputSignalLocked ? "true" : "false")
                << ",\"reference_locked\":" << (status.referenceLocked ? "true" : "false")
                << ",\"capture_busy\":" << (status.captureBusy ? "true" : "false")
                << ",\"playback_busy\":" << (status.playbackBusy ? "true" : "false") << ",\"pcie_link_width\":" << jnum(status.pcieLinkWidth)
                << ",\"pcie_link_speed\":" << jnum(status.pcieLinkSpeed) << ",\"temperature_c\":" << jnum(status.temperatureC) << "}}";
        }
        out << "]}";
        return jsonResponse(200, out.str());
    }

    HttpResponse WebService::apiConfigGet()
    {
        std::ostringstream out;
        out << "{";
        out << "\"file\":" << jopt(_store.filePath());
        out << ",\"restart_required\":" << (_restartRequired.load() ? "true" : "false");

        // Schema (for the UI to build forms).
        out << ",\"schema\":[";
        bool first = true;
        for (auto const& meta : config::settingsSchema())
        {
            if (!first)
            {
                out << ",";
            }
            first = false;
            out << "{\"key\":" << jstr(meta.key) << ",\"kind\":"
                << jstr(meta.kind == config::SettingKind::Global       ? "global"
                          : meta.kind == config::SettingKind::AudioFlow ? "audio_flow"
                                                                        : "channel")
                << ",\"type\":" << jstr(meta.type) << ",\"default\":" << jstr(meta.defaultValue) << ",\"help\":" << jstr(meta.help)
                << ",\"requires_restart\":" << (meta.requiresRestart ? "true" : "false") << ",\"options\":[";
            for (std::size_t i = 0; i < meta.options.size(); ++i)
            {
                if (i > 0)
                {
                    out << ",";
                }
                out << jstr(meta.options[i]);
            }
            out << "]}";
        }
        out << "]";

        // Present values with provenance (§7.5.2).
        out << ",\"values\":{";
        first = true;
        for (auto const& key : _store.presentKeys())
        {
            auto const value = _store.effectiveValue(key);
            if (!value)
            {
                continue;
            }
            if (!first)
            {
                out << ",";
            }
            first = false;
            auto const source = _store.sourceOf(key);
            out << jstr(key) << ":{\"value\":" << jstr(*value) << ",\"source\":" << jstr(config::settingSourceName(source))
                << ",\"editable\":" << (source != config::SettingSource::Env && _store.hasFileLayer() ? "true" : "false") << "}";
        }
        out << "}";

        out << ",\"env_block\":" << jstr(_store.renderEnvBlock());
        out << ",\"card_subdevices\":" << _card.subDeviceCount();
        out << "}";
        return jsonResponse(200, out.str());
    }

    HttpResponse WebService::apiConfigPut(HttpRequest const& req)
    {
        picojson::value root;
        std::string const err = picojson::parse(root, req.body);
        if (!err.empty() || !root.is<picojson::object>())
        {
            return jsonError(400, "body must be a JSON object: {\"set\": {KEY: value}, \"unset\": [KEY]}");
        }

        std::map<std::string, std::optional<std::string>> changes;
        auto const& obj = root.get<picojson::object>();
        if (auto const it = obj.find("set"); it != obj.end())
        {
            if (!it->second.is<picojson::object>())
            {
                return jsonError(400, "'set' must be an object of KEY: value pairs");
            }
            for (auto const& [key, value] : it->second.get<picojson::object>())
            {
                std::string rendered;
                if (value.is<std::string>())
                {
                    rendered = value.get<std::string>();
                }
                else if (value.is<bool>())
                {
                    rendered = value.get<bool>() ? "true" : "false";
                }
                else if (value.is<double>())
                {
                    // Match the config-file loader: only whole numbers (picojson
                    // has no integer type; reject 1.5 rather than truncating).
                    double const d = value.get<double>();
                    if (d != static_cast<double>(static_cast<long long>(d)))
                    {
                        return jsonError(400, "value for '" + key + "' must be a string/integer/bool");
                    }
                    rendered = std::to_string(static_cast<long long>(d));
                }
                else
                {
                    return jsonError(400, "value for '" + key + "' must be a string/integer/bool");
                }
                changes[key] = rendered;
            }
        }
        if (auto const it = obj.find("unset"); it != obj.end())
        {
            if (!it->second.is<picojson::array>())
            {
                return jsonError(400, "'unset' must be an array of keys");
            }
            for (auto const& k : it->second.get<picojson::array>())
            {
                if (!k.is<std::string>())
                {
                    return jsonError(400, "'unset' entries must be strings");
                }
                changes[k.get<std::string>()] = std::nullopt;
            }
        }
        if (changes.empty())
        {
            return jsonError(400, "nothing to change");
        }

        auto result = _store.update(changes);
        if (std::holds_alternative<std::string>(result))
        {
            return jsonError(422, std::get<std::string>(result));
        }
        auto& update = std::get<config::ConfigStore::UpdateResult>(result);

        // §7.5.3: apply per-channel changes live; flag global changes.
        bool restartRequired = !config::globalPartEquals(update.config, _activeCfg);

        auto applyResult = _channels.applyChannels(update.config.channels);
        if (std::holds_alternative<std::string>(applyResult))
        {
            // Persisted but not applicable to this card — surface clearly.
            return jsonError(422, std::get<std::string>(applyResult) + " (persisted to file, but not applied)");
        }
        auto const& applied = std::get<channel::ChannelManager::ApplyResult>(applyResult);
        if (restartRequired)
        {
            _restartRequired.store(true);
        }

        std::ostringstream out;
        out << "{\"ok\":true,\"restart_required\":" << (_restartRequired.load() ? "true" : "false");
        auto emitList = [&out](char const* name, std::vector<int> const& list) {
            out << ",\"" << name << "\":[";
            for (std::size_t i = 0; i < list.size(); ++i)
            {
                if (i > 0)
                {
                    out << ",";
                }
                out << list[i];
            }
            out << "]";
        };
        emitList("channels_added", applied.added);
        emitList("channels_restarted", applied.restarted);
        emitList("channels_removed", applied.removed);
        out << "}";
        return jsonResponse(200, out.str());
    }

    HttpResponse WebService::apiDomainsGet()
    {
        auto const domains = mxlbridge::scanDomains(_activeCfg.domainScanPath);
        std::ostringstream out;
        out << "{\"scan_path\":" << jstr(_activeCfg.domainScanPath) << ",\"domains\":[";
        bool first = true;
        for (auto const& d : domains)
        {
            if (!first)
            {
                out << ",";
            }
            first = false;
            out << "{\"path\":" << jstr(d.path) << ",\"id\":" << jopt(d.id) << ",\"label\":" << jopt(d.label)
                << ",\"description\":" << jopt(d.description) << ",\"tmpfs\":" << (d.isTmpfs ? "true" : "false")
                << ",\"flow_count\":" << d.flowCount << ",\"history_duration_ns\":" << jnum(d.historyDurationNs)
                << ",\"current\":" << (d.path == _domain.path() ? "true" : "false") << "}";
        }
        out << "]}";
        return jsonResponse(200, out.str());
    }

    HttpResponse WebService::apiDomainsPost(HttpRequest const& req)
    {
        picojson::value root;
        std::string const err = picojson::parse(root, req.body);
        if (!err.empty() || !root.is<picojson::object>())
        {
            return jsonError(400, "body must be a JSON object: {path, label, description?, history_ms?}");
        }
        auto const& obj = root.get<picojson::object>();
        auto getStr = [&obj](char const* key) -> std::optional<std::string> {
            auto const it = obj.find(key);
            if (it == obj.end() || !it->second.is<std::string>())
            {
                return std::nullopt;
            }
            return it->second.get<std::string>();
        };

        mxlbridge::CreateDomainRequest request;
        auto const path = getStr("path");
        if (!path || path->empty())
        {
            return jsonError(400, "'path' is required (absolute, inside the scan root)");
        }
        request.path = *path;
        request.label = getStr("label").value_or("");
        request.description = getStr("description").value_or("");
        if (auto const it = obj.find("history_ms"); it != obj.end() && it->second.is<double>())
        {
            request.historyDurationNs = static_cast<std::uint64_t>(it->second.get<double>() * 1'000'000.0);
        }

        auto result = mxlbridge::createDomain(request, _activeCfg.domainScanPath);
        if (std::holds_alternative<std::string>(result))
        {
            return jsonError(422, std::get<std::string>(result));
        }
        auto const& created = std::get<mxlbridge::CreateDomainResult>(result);
        std::ostringstream out;
        out << "{\"ok\":true,\"path\":" << jstr(created.path) << ",\"id\":" << jstr(created.id)
            << ",\"tmpfs\":" << (created.isTmpfs ? "true" : "false") << "}";
        return jsonResponse(200, out.str());
    }

    HttpResponse WebService::apiFlowsGet(HttpRequest const& req)
    {
        auto const rawDomain = req.queryParam("domain").value_or(_domain.path());
        // Containment: only the active domain, or a path that canonicalizes
        // under the scan root (rejects `..` traversal and non-segment prefixes).
        std::error_code ec;
        auto const domainCanon = std::filesystem::weakly_canonical(rawDomain, ec);
        if (ec || domainCanon.empty())
        {
            return jsonError(400, "invalid domain path");
        }
        auto const domain = domainCanon.string();
        auto const activeCanon = std::filesystem::weakly_canonical(_domain.path(), ec);
        bool const isActive = !ec && !activeCanon.empty() && domainCanon == activeCanon;
        if (!isActive && !mxlbridge::pathIsUnderRoot(domain, _activeCfg.domainScanPath))
        {
            return jsonError(400, "domain must be inside MXL_DOMAIN_SCAN_PATH");
        }

        auto const flows = mxlbridge::listFlows(domain);
        std::ostringstream out;
        out << "{\"domain\":" << jstr(domain) << ",\"flows\":[";
        bool first = true;
        for (auto const& f : flows)
        {
            if (!first)
            {
                out << ",";
            }
            first = false;
            out << "{\"id\":" << jstr(f.id) << ",\"format\":" << jstr(f.format) << ",\"media_type\":" << jstr(f.mediaType)
                << ",\"label\":" << jstr(f.label) << ",\"group_hint\":" << jstr(f.groupHint) << ",\"width\":" << jnum(f.width)
                << ",\"height\":" << jnum(f.height) << ",\"rate_numerator\":" << jnum(f.rateNumerator)
                << ",\"rate_denominator\":" << jnum(f.rateDenominator) << ",\"interlaced\":" << (f.interlaced ? "true" : "false")
                << ",\"channel_count\":" << jnum(f.channelCount) << ",\"active\":" << (f.active ? "true" : "false")
                << ",\"head_index\":" << jnum(f.headIndex) << ",\"last_write_time_ns\":" << jnum(f.lastWriteTimeNs)
                << ",\"grain_count\":" << jnum(f.grainCount) << "}";
        }
        out << "]}";
        return jsonResponse(200, out.str());
    }
}
