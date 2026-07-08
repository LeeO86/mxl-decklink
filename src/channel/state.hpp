// SPDX-License-Identifier: MIT
// Channel state model (SPECIFICATION.md §7.2) and the process-global channel
// state table (§3.7).
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

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
        std::atomic<std::uint64_t> framesDropped{0};
        std::atomic<std::uint64_t> reconnects{0};
        std::atomic<std::uint64_t> grainsCommitted{0};

        // §3.8: the currently active video flow UUID (may differ from the
        // configured one after a format change). Guarded by a mutex — read
        // rarely (statusz/metrics), written on format changes only.
        std::string activeVideoFlowId() const
        {
            std::lock_guard const lock{_flowIdMutex};
            return _activeVideoFlowId;
        }

        void setActiveVideoFlowId(std::string id)
        {
            std::lock_guard const lock{_flowIdMutex};
            _activeVideoFlowId = std::move(id);
        }

    private:
        mutable std::mutex _flowIdMutex;
        std::string _activeVideoFlowId;
    };
}
