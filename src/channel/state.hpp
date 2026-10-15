// SPDX-License-Identifier: MIT
// Channel state model (SPECIFICATION.md §7.2) and the process-global channel
// state table (§3.7).
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "decklink/types.hpp"

namespace mxldl::channel
{
    enum class State : int
    {
        Init = 0,
        Healthy = 1,
        Degraded = 2,
        Failed = 3,
    };

    inline char const* stateName(State s)
    {
        switch (s)
        {
            case State::Init: return "init";
            case State::Healthy: return "healthy";
            case State::Degraded: return "degraded";
            case State::Failed: return "failed";
        }
        return "?";
    }

    /// Live status snapshot published by every channel; consumed by /readyz,
    /// /statusz and metrics (§7). All fields are updated lock-free from the
    /// channel's own threads.
    struct Status
    {
        std::atomic<State> state{State::Init};
        std::atomic<std::uint64_t> lastFrameTaiNs{0};
        std::atomic<bool> signalLocked{false};
        std::atomic<std::uint64_t> framesTotal{0};
        /// Inputs: frames lost. Outputs: frames the card dropped or that
        /// could not be scheduled (output-side trouble).
        std::atomic<std::uint64_t> framesDropped{0};
        /// Outputs: frames that repeated the last frame because no new grain
        /// could be read (source stall, flow re-open, read error).
        std::atomic<std::uint64_t> framesRepeated{0};
        /// Outputs: CHx_READ_OFFSET_GRAINS in effect (capped at bring-up).
        std::atomic<int> readOffsetGrains{0};
        std::atomic<std::uint64_t> reconnects{0};
        std::atomic<std::uint64_t> grainsCommitted{0};
        /// Output only: DeckLink `GetBufferedAudioSampleFrameCount` (sample
        /// frames, not bytes). Zero on inputs / when not streaming.
        std::atomic<std::uint32_t> bufferedAudioFrames{0};
        /// Output only: DeckLink buffered video frame count.
        std::atomic<std::uint32_t> bufferedVideoFrames{0};

        // §3.8: the currently active video flow UUID (may differ from the
        // configured one after a format change). Guarded by a mutex — read
        // rarely (statusz/metrics), written on format changes only.
        std::string activeVideoFlowId() const
        {
            std::lock_guard const lock{_stringMutex};
            return _activeVideoFlowId;
        }

        void setActiveVideoFlowId(std::string id)
        {
            std::lock_guard const lock{_stringMutex};
            _activeVideoFlowId = std::move(id);
        }

        /// The video mode the channel currently runs (auto-detect updates it).
        std::string activeModeName() const
        {
            std::lock_guard const lock{_stringMutex};
            return _activeModeName;
        }

        void setActiveModeName(std::string name)
        {
            std::lock_guard const lock{_stringMutex};
            _activeModeName = std::move(name);
        }

        /// Outputs: the card's SDI output signalling, read at bring-up.
        dl::SdiOutputConfig sdiOutput() const
        {
            std::lock_guard const lock{_stringMutex};
            return _sdiOutput;
        }

        void setSdiOutput(dl::SdiOutputConfig c)
        {
            std::lock_guard const lock{_stringMutex};
            _sdiOutput = std::move(c);
        }

    private:
        mutable std::mutex _stringMutex;
        dl::SdiOutputConfig _sdiOutput;
        std::string _activeVideoFlowId;
        std::string _activeModeName;
    };
}
