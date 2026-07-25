// SPDX-License-Identifier: MIT
// Web control interface: REST API + embedded UI (SPECIFICATION.md §7.5).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "channel/channel_manager.hpp"
#include "config/config.hpp"
#include "config/store.hpp"
#include "decklink/device.hpp"
#include "mxlbridge/domain.hpp"
#include "ops/health.hpp"
#include "ops/httpserver.hpp"

namespace mxldl::ops
{
    /// The consolidated HTTP server on WEB_PORT (§7.1): health endpoints and
    /// /metrics are always served; the web UI and /api/… only when
    /// WEB_ENABLE=true (§7.5).
    class WebService
    {
    public:
        /// `activeCfg` is the configuration this process started with (used
        /// for restart_required detection, §7.5.3).
        WebService(config::Config const& activeCfg, config::ConfigStore& store, channel::ChannelManager& channels, dl::ICard& card,
            mxlbridge::Domain& domain, HealthService& health);

        void start(); // throws on bind failure
        void stop();

        [[nodiscard]] bool restartRequired() const
        {
            return _restartRequired.load();
        }

    private:
        HttpResponse handle(HttpRequest const& req);
        HttpResponse apiStatus();
        HttpResponse apiCard();
        HttpResponse apiConfigGet();
        HttpResponse apiConfigPut(HttpRequest const& req);
        HttpResponse apiDomainsGet();
        HttpResponse apiDomainsPost(HttpRequest const& req);
        HttpResponse apiFlowsGet(HttpRequest const& req);

        config::Config const& _activeCfg;
        config::ConfigStore& _store;
        channel::ChannelManager& _channels;
        dl::ICard& _card;
        mxlbridge::Domain& _domain;
        HealthService& _health;
        std::atomic<bool> _restartRequired{false};
        std::uint64_t _startedAtTaiNs = 0;
        std::unique_ptr<HttpServer> _server;
    };
}
