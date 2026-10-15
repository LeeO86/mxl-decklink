// SPDX-License-Identifier: MIT
#include "flowsync.hpp"

#include <stdexcept>

namespace mxldl::mxlbridge
{
    FlowSyncGroup::FlowSyncGroup(std::string const& domainPath)
        : _instance(::mxlCreateInstance(domainPath.c_str(), nullptr))
    {
        if (_instance == nullptr)
        {
            throw std::runtime_error("mxlCreateInstance failed for the sync group of domain: " + domainPath);
        }
        auto const status = ::mxlCreateFlowSynchronizationGroup(_instance, &_group);
        if (status != MXL_STATUS_OK || _group == nullptr)
        {
            ::mxlDestroyInstance(_instance);
            throw std::runtime_error("mxlCreateFlowSynchronizationGroup failed with status " + std::to_string(status));
        }
    }

    FlowSyncGroup::~FlowSyncGroup()
    {
        ::mxlReleaseFlowSynchronizationGroup(_instance, _group);
        ::mxlDestroyInstance(_instance);
    }

    void FlowSyncGroup::addReader(mxlFlowReader reader)
    {
        auto const status = ::mxlFlowSynchronizationGroupAddReader(_group, reader);
        if (status != MXL_STATUS_OK)
        {
            throw std::runtime_error("mxlFlowSynchronizationGroupAddReader failed with status " + std::to_string(status));
        }
    }

    void FlowSyncGroup::removeReader(mxlFlowReader reader)
    {
        (void)::mxlFlowSynchronizationGroupRemoveReader(_group, reader);
    }

    mxlStatus FlowSyncGroup::waitFor(std::uint64_t timestampNs, std::uint64_t timeoutNs)
    {
        return ::mxlFlowSynchronizationGroupWaitForDataAt(_group, timestampNs, timeoutNs);
    }
}
