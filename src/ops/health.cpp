// SPDX-License-Identifier: MIT
#include "health.hpp"

#include "util/logging.hpp"
#include "util/taiclock.hpp"

namespace mxldl::ops
{
    HealthService::HealthService(config::Config const& cfg, channel::ChannelManager& channels, Registry& metrics)
        : _cfg(cfg)
        , _channels(channels)
        , _metrics(metrics)
    {
        heartbeat();
    }

    void HealthService::heartbeat()
    {
        _lastHeartbeatTai.store(util::taiNowNs());
    }

    HttpResponse HealthService::metricsText()
    {
        return {200, "text/plain; version=0.0.4; charset=utf-8", _metrics.render()};
    }

    HttpResponse HealthService::livez()
    {
        // §7.1: healthy while the housekeeping thread was active < 5 s ago.
        auto const last = _lastHeartbeatTai.load();
        auto const now = util::taiNowNs();
        if (now - last > 5'000'000'000ULL)
        {
            return {503, "text/plain; charset=utf-8", "housekeeping thread stalled\n"};
        }
        return {200, "text/plain; charset=utf-8", "ok\n"};
    }

    HttpResponse HealthService::readyz()
    {
        auto const healthy = _channels.healthyCount();
        if (healthy >= _cfg.minHealthyChannels)
        {
            return {200, "text/plain; charset=utf-8", "ok\n"};
        }

        // §7.1: 503 with a JSON body listing per-channel state.
        std::string body = "{\"healthy_channels\":" + std::to_string(healthy) +
                           ",\"required\":" + std::to_string(_cfg.minHealthyChannels) + ",\"channels\":[";
        bool first = true;
        for (auto const& v : _channels.channels())
        {
            if (!first)
            {
                body += ',';
            }
            first = false;
            body += "{\"index\":" + std::to_string(v.cfg.index) + ",\"label\":\"" + log::jsonEscape(v.cfg.label) + "\",\"state\":\"" +
                    channel::stateName(v.state) + "\"}";
        }
        body += "]}";
        return {503, "application/json", body};
    }

    HttpResponse HealthService::statusz()
    {
        // §7.1: full report — always 200.
        std::string body = "{\"channels\":[";
        bool first = true;
        for (auto const& v : _channels.channels())
        {
            if (!first)
            {
                body += ',';
            }
            first = false;
            body += "{\"index\":" + std::to_string(v.cfg.index);
            body += ",\"label\":\"" + log::jsonEscape(v.cfg.label) + "\"";
            body += ",\"direction\":\"" + std::string(config::directionName(v.cfg.direction)) + "\"";
            body += ",\"subdevice_index\":" + std::to_string(v.cfg.subdeviceIndex);
            body += ",\"state\":\"" + std::string(channel::stateName(v.state)) + "\"";
            body += ",\"signal_locked\":" + std::string(v.signalLocked ? "true" : "false");
            body += ",\"active_video_mode\":\"" + log::jsonEscape(v.activeModeName) + "\"";
            body += ",\"last_frame_tai_ns\":" + std::to_string(v.lastFrameTaiNs);
            body += ",\"frames_total\":" + std::to_string(v.framesTotal);
            body += ",\"frames_dropped\":" + std::to_string(v.framesDropped);
            body += ",\"reconnects\":" + std::to_string(v.reconnects);
            body += ",\"grains_committed\":" + std::to_string(v.grainsCommitted);
            body += ",\"active_video_flow_id\":\"" + v.activeVideoFlowId + "\"";
            body += ",\"audio\":{";
            body += "\"enabled\":" + std::string(v.cfg.audioEnable ? "true" : "false");
            body += ",\"deck_channel_count\":" + std::to_string(v.cfg.audioChannelCount);
            body += ",\"flow_count\":" + std::to_string(v.cfg.audioFlows.size());
            if (v.cfg.direction == config::Direction::Output)
            {
                body += ",\"decklink_buffered_audio_frames\":" + std::to_string(v.bufferedAudioFrames);
            }
            else
            {
                body += ",\"decklink_buffered_audio_frames\":null";
            }
            body += ",\"flows\":[";
            bool firstAf = true;
            for (auto const& af : v.cfg.audioFlows)
            {
                if (!firstAf)
                {
                    body += ',';
                }
                firstAf = false;
                body += "{\"index\":" + std::to_string(af.index);
                body += ",\"flow_id\":\"" + af.flowId.toString() + "\"";
                body += ",\"channel_count\":" + std::to_string(af.channelCount);
                body += ",\"unassigned\":" + std::string(af.flowId.isNil() ? "true" : "false");
                body += ",\"map\":[";
                for (std::size_t i = 0; i < af.deckLinkChannels.size(); ++i)
                {
                    if (i > 0)
                    {
                        body += ',';
                    }
                    body += std::to_string(af.deckLinkChannels[i]);
                }
                body += "]}";
            }
            body += "]}";
            body += "}";
        }
        body += "]}";
        return {200, "application/json", body};
    }
}
