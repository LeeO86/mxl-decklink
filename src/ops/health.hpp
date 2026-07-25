// SPDX-License-Identifier: MIT
// Health endpoints /livez /readyz /statusz and /metrics (SPECIFICATION.md
// §7.1). Since v1.2 these are served by the consolidated HTTP server on
// WEB_PORT (ops/webapi.*); this class only provides the responses and the
// liveness heartbeat.
#pragma once

#include <atomic>
#include <cstdint>

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

        /// Updated by the housekeeping thread; /livez requires activity
        /// within the last 5 seconds (§7.1).
        void heartbeat();

        HttpResponse livez();
        HttpResponse readyz();
        HttpResponse statusz();
        HttpResponse metricsText();

    private:
        config::Config const& _cfg;
        channel::ChannelManager& _channels;
        Registry& _metrics;
        std::atomic<std::uint64_t> _lastHeartbeatTai{0};
    };
}
