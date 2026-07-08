// SPDX-License-Identifier: MIT
// Health endpoints /livez /readyz /statusz (SPECIFICATION.md §7.1).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "channel/channel_manager.hpp"
#include "config/config.hpp"
#include "ops/httpserver.hpp"
#include "ops/metrics.hpp"

namespace mxldl::ops
{
    class HealthService
    {
    public:
        HealthService(config::Config const& cfg, channel::ChannelManager& channels, Registry& metrics);

        /// Binds both listeners; throws std::runtime_error on failure.
        void start();
        void stop();

        /// Updated by the housekeeping thread; /livez requires activity
        /// within the last 5 seconds (§7.1).
        void heartbeat();

    private:
        HttpResponse handleHealth(std::string const& path);
        HttpResponse handleMetrics(std::string const& path);
        HttpResponse livez();
        HttpResponse readyz();
        HttpResponse statusz();

        config::Config const& _cfg;
        channel::ChannelManager& _channels;
        Registry& _metrics;
        std::atomic<std::uint64_t> _lastHeartbeatTai{0};
        std::unique_ptr<HttpServer> _healthServer;
        std::unique_ptr<HttpServer> _metricsServer;
    };
}
