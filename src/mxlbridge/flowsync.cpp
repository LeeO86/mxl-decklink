// SPDX-License-Identifier: MIT
#include "flowsync.hpp"

#include <stdexcept>

namespace mxldl::mxlbridge
{
    FlowSyncGroup::FlowSyncGroup(Domain& domain)
        : _domain(domain)
    {
        auto const status = ::mxlCreateFlowSynchronizationGroup(domain.instance(), &_group);
        if (status != MXL_STATUS_OK || _group == nullptr)
        {
            _group = nullptr;
            throw std::runtime_error("mxlCreateFlowSynchronizationGroup failed with status " + std::to_string(status));
        }
    }

    FlowSyncGroup::~FlowSyncGroup()
    {
        if (_group != nullptr)
        {
            ::mxlReleaseFlowSynchronizationGroup(_domain.instance(), _group);
        }
    }

    void FlowSyncGroup::addReader(mxlFlowReader reader)
    {
        auto const status = ::mxlFlowSynchronizationGroupAddReader(_group, reader);
        if (status != MXL_STATUS_OK)
        {
            throw std::runtime_error("mxlFlowSynchronizationGroupAddReader failed with status " + std::to_string(status));
        }
    }

    mxlStatus FlowSyncGroup::waitFor(std::uint64_t timestampNs, std::uint64_t timeoutNs)
    {
        return ::mxlFlowSynchronizationGroupWaitForDataAt(_group, timestampNs, timeoutNs);
    }
}
