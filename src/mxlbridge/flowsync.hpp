// SPDX-License-Identifier: MIT
// RAII wrapper for mxlFlowSynchronizationGroup (MXL v1.1).
#pragma once

#include <cstdint>

#include <mxl/flow.h>
#include <mxl/mxl.h>

#include "mxlbridge/domain.hpp"

namespace mxldl::mxlbridge
{
    /// Waits until every added reader has data for one TAI timestamp.
    /// Video readers wait for a full grain; audio readers wait for the sample
    /// at that time. The group does not own the readers.
    class FlowSyncGroup
    {
    public:
        explicit FlowSyncGroup(Domain& domain);
        ~FlowSyncGroup();

        FlowSyncGroup(FlowSyncGroup const&) = delete;
        FlowSyncGroup& operator=(FlowSyncGroup const&) = delete;

        void addReader(mxlFlowReader reader);

        /// `timestampNs` is TAI nanoseconds since the SMPTE ST 2059 epoch
        /// (`mxlIndexToTimestamp`).
        [[nodiscard]] mxlStatus waitFor(std::uint64_t timestampNs, std::uint64_t timeoutNs);

    private:
        Domain& _domain;
        mxlFlowSynchronizationGroup _group = nullptr;
    };
}
