// SPDX-License-Identifier: MIT
// Owns all channels of the card; aggregates state for health (SPECIFICATION.md
// §3.7, §7.2).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "channel/input_channel.hpp"
#include "channel/output_channel.hpp"
#include "config/config.hpp"
#include "decklink/device.hpp"
#include "mxlbridge/domain.hpp"
#include "ops/metrics.hpp"

namespace mxldl::channel
{
    class ChannelManager
    {
    public:
        ChannelManager(config::Config const& cfg, dl::ICard& card, mxlbridge::Domain& domain, ops::Registry& metrics);

        void startAll();
        void stopAll();

        /// Housekeeping tick for all channels (§2.5).
        void housekeeping();

        struct ChannelView
        {
            config::ChannelConfig const* cfg;
            Status const* status;
        };

        [[nodiscard]] std::vector<ChannelView> channels() const;
        [[nodiscard]] int healthyCount() const;
        [[nodiscard]] std::size_t totalCount() const;

    private:
        std::vector<std::unique_ptr<InputChannel>> _inputs;
        std::vector<std::unique_ptr<OutputChannel>> _outputs;
    };
}
