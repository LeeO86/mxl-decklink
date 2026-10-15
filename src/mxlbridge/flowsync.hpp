// SPDX-License-Identifier: MIT
// RAII wrapper for mxlFlowSynchronizationGroup (MXL v1.1).
#pragma once

#include <cstdint>
#include <string>

#include <mxl/flow.h>
#include <mxl/mxl.h>

namespace mxldl::mxlbridge
{
    /// Waits until every added reader has data for one TAI timestamp.
    /// Video readers wait for a full grain; audio readers wait for the sample
    /// at that time. The group does not own the readers.
    ///
    /// The group lives on a private mxlInstance of the domain that has no
    /// readers. dmf-mxl/mxl@218ddaa lib/internal/src/Instance.cpp:145-165,
    /// 439-455: releasing a reader's last reference calls removeReader on every
    /// sync group of the reader's instance, while creating and releasing a
    /// group change that list without the instance mutex, and a group has no
    /// lock. On the shared domain instance, another channel releasing a reader
    /// could walk a group being created or released (SIGSEGV in
    /// FlowSynchronizationGroup::removeReader). AddReader accepts readers of
    /// any instance (lib/src/flow.cpp:702-730).
    class FlowSyncGroup
    {
    public:
        explicit FlowSyncGroup(std::string const& domainPath);
        ~FlowSyncGroup();

        FlowSyncGroup(FlowSyncGroup const&) = delete;
        FlowSyncGroup& operator=(FlowSyncGroup const&) = delete;

        void addReader(mxlFlowReader reader);

        /// The group's instance holds no readers, so MXL does not take a
        /// released reader out of this group: remove it before releasing it.
        void removeReader(mxlFlowReader reader);

        /// `timestampNs` is TAI nanoseconds since the SMPTE ST 2059 epoch
        /// (`mxlIndexToTimestamp`).
        [[nodiscard]] mxlStatus waitFor(std::uint64_t timestampNs, std::uint64_t timeoutNs);

    private:
        mxlInstance _instance = nullptr;
        mxlFlowSynchronizationGroup _group = nullptr;
    };
}
