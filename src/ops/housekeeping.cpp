// SPDX-License-Identifier: MIT
#include "housekeeping.hpp"

#include <chrono>

#include "util/threading.hpp"

namespace mxldl::ops
{
    Housekeeping::Housekeeping(config::Config const& cfg, channel::ChannelManager& channels, mxlbridge::Domain& domain, HealthService& health,
        Registry& metrics)
        : _cfg(cfg)
        , _channels(channels)
        , _domain(domain)
        , _health(health)
        , _metrics(metrics)
    {}

    Housekeeping::~Housekeeping()
    {
        stop();
    }

    void Housekeeping::start()
    {
        _running.store(true);
        _thread = std::thread([this] {
            util::setThreadName("housekeeping");
            loop();
        });
    }

    void Housekeeping::stop()
    {
        _running.store(false);
        if (_thread.joinable())
        {
            _thread.join();
        }
    }

    void Housekeeping::loop()
    {
        int gcCountdown = 0;
        while (_running.load())
        {
            _health.heartbeat();
            _channels.housekeeping();

            // §3.8: publish the active video flow UUID as an info metric.
            for (auto const& v : _channels.channels())
            {
                Labels labels = {
                    {"channel_index", std::to_string(v.cfg->index)},
                    {"channel_label", v.cfg->label},
                    {"direction", config::directionName(v.cfg->direction)},
                    {"flow_id", v.status->activeVideoFlowId()},
                };
                _metrics.gauge("mxl_active_video_flow_id", "Active video flow UUID (info metric, value is always 1)", labels).set(1.0);
            }

            // §2.2/mxl.h: periodic stale-flow garbage collection (~60 s).
            if (--gcCountdown <= 0)
            {
                _domain.garbageCollect();
                gcCountdown = 60;
            }

            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}
