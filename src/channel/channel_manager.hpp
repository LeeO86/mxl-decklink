// SPDX-License-Identifier: MIT
// Owns all channels of the card; aggregates state for health (SPECIFICATION.md
// §3.7, §7.2) and applies runtime per-channel reconfiguration (§7.5.3).
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
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
        /// Takes a snapshot of the global configuration (stable storage for
        /// the channels' back-references); the channel list from `cfg` forms
        /// the initial set.
        ChannelManager(config::Config const& cfg, dl::ICard& card, mxlbridge::Domain& domain, ops::Registry& metrics);
        ~ChannelManager();

        void startAll();
        void stopAll();

        struct ApplyResult
        {
            std::vector<int> added;
            std::vector<int> restarted;
            std::vector<int> removed;
        };

        /// Diffs `channels` against the running set by channel index and
        /// stops/starts only what changed (§7.5.3). The list must already be
        /// §4.3-validated; card capability checks are performed here and
        /// returned as an error string without touching any channel.
        std::variant<ApplyResult, std::string> applyChannels(std::vector<config::ChannelConfig> const& channels);

        /// Housekeeping tick for all channels (§2.5).
        void housekeeping();

        struct ChannelView
        {
            config::ChannelConfig cfg;
            State state = State::Init;
            bool signalLocked = false;
            std::uint64_t lastFrameTaiNs = 0;
            std::uint64_t framesTotal = 0;
            std::uint64_t framesDropped = 0;
            std::uint64_t reconnects = 0;
            std::uint64_t grainsCommitted = 0;
            std::uint32_t bufferedAudioFrames = 0;
            std::uint32_t bufferedVideoFrames = 0;
            std::string activeVideoFlowId;
            std::string activeModeName;
        };

        [[nodiscard]] std::vector<ChannelView> channels() const;
        [[nodiscard]] int healthyCount() const;
        [[nodiscard]] std::size_t totalCount() const;

    private:
        struct Entry
        {
            config::ChannelConfig cfg;
            std::unique_ptr<InputChannel> input;
            std::unique_ptr<OutputChannel> output;
        };

        void startEntry(Entry& entry);
        static void stopEntry(Entry& entry);
        std::optional<std::string> validateAgainstCard(config::ChannelConfig const& ch) const;

        config::Config _globalCfg; // stable snapshot (global part only used)
        dl::ICard& _card;
        mxlbridge::Domain& _domain;
        ops::Registry& _metrics;

        mutable std::mutex _mutex;
        std::map<int, Entry> _entries;
        bool _started = false;
    };
}
