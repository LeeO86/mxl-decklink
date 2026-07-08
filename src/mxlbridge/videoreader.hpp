// SPDX-License-Identifier: MIT
// Video grain reading for the output path (SPECIFICATION.md §2.4).
#pragma once

#include <cstdint>
#include <string>

#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/rational.h>

#include "mxlbridge/domain.hpp"

namespace mxldl::mxlbridge
{
    class VideoReader
    {
    public:
        /// Opens a reader on an existing flow. Throws on failure (typically
        /// MXL_ERR_FLOW_NOT_FOUND while the producer is not up yet — callers
        /// retry per §3.7).
        VideoReader(Domain& domain, std::string flowId);
        ~VideoReader();

        VideoReader(VideoReader const&) = delete;
        VideoReader& operator=(VideoReader const&) = delete;

        struct Grain
        {
            mxlGrainInfo info{};
            std::uint8_t const* payload = nullptr;
        };

        /// Blocking fetch with timeout (CHx_READER_TIMEOUT_MS).
        mxlStatus getGrain(std::uint64_t index, std::uint64_t timeoutNs, Grain& out);

        [[nodiscard]] mxlRational grainRate() const
        {
            return _configInfo.common.grainRate;
        }

        [[nodiscard]] std::uint32_t grainCount() const
        {
            return _configInfo.discrete.grainCount;
        }

        /// Current head index of the flow ring (for lag metrics §7.3).
        [[nodiscard]] std::uint64_t headIndex() const;

        [[nodiscard]] std::string const& flowId() const
        {
            return _flowId;
        }

    private:
        Domain& _domain;
        std::string _flowId;
        mxlFlowReader _reader = nullptr;
        mxlFlowConfigInfo _configInfo{};
    };
}
