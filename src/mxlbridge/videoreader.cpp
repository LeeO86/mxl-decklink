// SPDX-License-Identifier: MIT
#include "videoreader.hpp"

#include <stdexcept>

namespace mxldl::mxlbridge
{
    VideoReader::VideoReader(Domain& domain, std::string flowId)
        : _domain(domain)
        , _flowId(std::move(flowId))
    {
        auto const status = ::mxlCreateFlowReader(domain.instance(), _flowId.c_str(), nullptr, &_reader);
        if (status != MXL_STATUS_OK)
        {
            throw std::runtime_error("mxlCreateFlowReader (video " + _flowId + ") failed with status " + std::to_string(status));
        }
        if (::mxlFlowReaderGetConfigInfo(_reader, &_configInfo) != MXL_STATUS_OK)
        {
            ::mxlReleaseFlowReader(domain.instance(), _reader);
            _reader = nullptr;
            throw std::runtime_error("mxlFlowReaderGetConfigInfo (video " + _flowId + ") failed");
        }
    }

    VideoReader::~VideoReader()
    {
        if (_reader != nullptr)
        {
            ::mxlReleaseFlowReader(_domain.instance(), _reader);
        }
    }

    mxlStatus VideoReader::getGrain(std::uint64_t index, std::uint64_t timeoutNs, Grain& out)
    {
        std::uint8_t* payload = nullptr;
        auto const status = ::mxlFlowReaderGetGrain(_reader, index, timeoutNs, &out.info, &payload);
        out.payload = payload;
        return status;
    }

    std::uint64_t VideoReader::headIndex() const
    {
        mxlFlowRuntimeInfo info{};
        if (::mxlFlowReaderGetRuntimeInfo(_reader, &info) != MXL_STATUS_OK)
        {
            return 0;
        }
        return info.headIndex;
    }
}
