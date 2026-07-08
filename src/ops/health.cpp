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

    void HealthService::start()
    {
        _healthServer = std::make_unique<HttpServer>(
            _cfg.healthPort,
            [this](std::string const& path) {
                return handleHealth(path);
            },
            "http-health");
        _metricsServer = std::make_unique<HttpServer>(
            _cfg.metricsPort,
            [this](std::string const& path) {
                return handleMetrics(path);
            },
            "http-metrics");
        _healthServer->start();
        _metricsServer->start();
    }

    void HealthService::stop()
    {
        if (_healthServer)
        {
            _healthServer->stop();
        }
        if (_metricsServer)
        {
            _metricsServer->stop();
        }
    }

    void HealthService::heartbeat()
    {
        _lastHeartbeatTai.store(util::taiNowNs());
    }

    HttpResponse HealthService::handleHealth(std::string const& path)
    {
        if (path == "/livez")
        {
            return livez();
        }
        if (path == "/readyz")
        {
            return readyz();
        }
        if (path == "/statusz")
        {
            return statusz();
        }
        return {404, "text/plain; charset=utf-8", "not found; endpoints: /livez /readyz /statusz\n"};
    }

    HttpResponse HealthService::handleMetrics(std::string const& path)
    {
        if (path == "/metrics")
        {
            return {200, "text/plain; version=0.0.4; charset=utf-8", _metrics.render()};
        }
        return {404, "text/plain; charset=utf-8", "not found; endpoint: /metrics\n"};
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
            body += "{\"index\":" + std::to_string(v.cfg->index) + ",\"label\":\"" + log::jsonEscape(v.cfg->label) + "\",\"state\":\"" +
                    channel::stateName(v.status->state.load()) + "\"}";
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
            body += "{\"index\":" + std::to_string(v.cfg->index);
            body += ",\"label\":\"" + log::jsonEscape(v.cfg->label) + "\"";
            body += ",\"direction\":\"" + std::string(config::directionName(v.cfg->direction)) + "\"";
            body += ",\"subdevice_index\":" + std::to_string(v.cfg->subdeviceIndex);
            body += ",\"state\":\"" + std::string(channel::stateName(v.status->state.load())) + "\"";
            body += ",\"signal_locked\":" + std::string(v.status->signalLocked.load() ? "true" : "false");
            body += ",\"last_frame_tai_ns\":" + std::to_string(v.status->lastFrameTaiNs.load());
            body += ",\"frames_total\":" + std::to_string(v.status->framesTotal.load());
            body += ",\"frames_dropped\":" + std::to_string(v.status->framesDropped.load());
            body += ",\"reconnects\":" + std::to_string(v.status->reconnects.load());
            body += ",\"grains_committed\":" + std::to_string(v.status->grainsCommitted.load());
            body += ",\"active_video_flow_id\":\"" + v.status->activeVideoFlowId() + "\"";
            body += "}";
        }
        body += "]}";
        return {200, "application/json", body};
    }
}
