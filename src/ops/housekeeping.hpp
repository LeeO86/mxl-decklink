// SPDX-License-Identifier: MIT
// Housekeeping thread (SPECIFICATION.md §2.5): liveness heartbeat, per-channel
// polls, MXL garbage collection, active-flow info metric.
#pragma once

#include <atomic>
#include <thread>

#include "channel/channel_manager.hpp"
#include "config/config.hpp"
#include "mxlbridge/domain.hpp"
#include "ops/health.hpp"
#include "ops/metrics.hpp"

namespace mxldl::ops
{
    class Housekeeping
    {
    public:
        Housekeeping(config::Config const& cfg, channel::ChannelManager& channels, mxlbridge::Domain& domain, HealthService& health,
            Registry& metrics);
        ~Housekeeping();

        void start();
        void stop();

    private:
        void loop();

        config::Config const& _cfg;
        channel::ChannelManager& _channels;
        mxlbridge::Domain& _domain;
        HealthService& _health;
        Registry& _metrics;
        std::atomic<bool> _running{false};
        std::thread _thread;
    };
}
