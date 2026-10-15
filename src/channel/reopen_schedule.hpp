// SPDX-License-Identifier: MIT
// When an output channel re-opens MXL readers whose flow was re-created
// (MXL_ERR_FLOW_INVALID, SPECIFICATION.md §2.4).
#pragma once

#include <cstdint>

namespace mxldl::channel
{
    /// A writer that re-creates a flow under the same id gives it a new data
    /// file; MXL then answers a read past the old head with
    /// MXL_ERR_FLOW_INVALID (dmf-mxl/mxl@218ddaa
    /// lib/internal/src/PosixDiscreteFlowReader.cpp:96-104, 244-254). The
    /// channel re-opens the reader right away, and again after
    /// retryDelayNs() while the flow is missing. Only the supervisor thread
    /// uses this object.
    class ReopenSchedule
    {
    public:
        /// A flow that stays missing this long marks the channel degraded.
        static constexpr std::uint64_t kDegradedAfterNs = 1'000'000'000ULL;

        /// Delay after the n-th failed open (0-based): 100 ms, 200 ms,
        /// 500 ms, then 1 s.
        [[nodiscard]] static constexpr std::uint64_t retryDelayNs(std::uint32_t failures)
        {
            constexpr std::uint64_t kDelaysMs[] = {100, 200, 500, 1000};
            constexpr std::uint32_t n = sizeof(kDelaysMs) / sizeof(kDelaysMs[0]);
            return kDelaysMs[failures < n ? failures : n - 1] * 1'000'000ULL;
        }

        /// A reader saw its flow re-created. The next open is due now; an
        /// event already pending keeps its start time and failure count.
        void flowInvalid(std::uint64_t nowNs)
        {
            if (!_pending)
            {
                _pending = true;
                _sinceNs = nowNs;
                _failures = 0;
            }
            _nextNs = nowNs;
        }

        [[nodiscard]] bool pending() const
        {
            return _pending;
        }

        [[nodiscard]] bool due(std::uint64_t nowNs) const
        {
            return _pending && nowNs >= _nextNs;
        }

        /// At least one reader could not be opened (flow still missing).
        void openFailed(std::uint64_t nowNs)
        {
            _nextNs = nowNs + retryDelayNs(_failures);
            ++_failures;
        }

        /// Every invalid reader is open again, or the channel restarts.
        void clear()
        {
            _pending = false;
            _failures = 0;
        }

        [[nodiscard]] std::uint32_t failures() const
        {
            return _failures;
        }

        [[nodiscard]] std::uint64_t pendingForNs(std::uint64_t nowNs) const
        {
            return _pending && nowNs > _sinceNs ? nowNs - _sinceNs : 0;
        }

        [[nodiscard]] bool degraded(std::uint64_t nowNs) const
        {
            return _pending && pendingForNs(nowNs) >= kDegradedAfterNs;
        }

    private:
        bool _pending = false;
        std::uint64_t _sinceNs = 0;
        std::uint64_t _nextNs = 0;
        std::uint32_t _failures = 0;
    };
}
