// SPDX-License-Identifier: MIT
// In-process AMWA NMOS node (IS-04 v1.3, IS-05 v1.2, BCP-007-03).
// Compiled only when MXL_DECKLINK_NMOS is enabled.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "channel/channel_manager.hpp"
#include "config/config.hpp"

namespace mxldl::nmosnode
{
    class Node
    {
    public:
        Node(config::Config const& cfg, channel::ChannelManager& channels, std::uint32_t cardPersistentId, std::string cardName);
        ~Node();

        Node(Node const&) = delete;
        Node& operator=(Node const&) = delete;

        /// Called after an IS-05 activation changes channel configuration, so
        /// the caller can persist it under CONFIG_DIR.
        void setPersist(std::function<void()> persist);

        /// Binds the Node and Connection APIs and registers with a registry when
        /// one is configured. Throws std::runtime_error on bind or setup failure.
        void start();

        /// True after the Registration API has accepted this node.
        [[nodiscard]] bool registered() const;

        /// Removes node resources so nmos-cpp sends Registration DELETEs, then stops.
        void stop();

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
