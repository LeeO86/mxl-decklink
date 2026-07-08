// SPDX-License-Identifier: MIT
// Video grain writing (SPECIFICATION.md §2.3).
#pragma once

#include <cstdint>
#include <string>

#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/rational.h>

#include "mxlbridge/domain.hpp"
#include "mxlbridge/flowdef.hpp"

namespace mxldl::mxlbridge
{
    class VideoWriter
    {
    public:
        /// Creates (or opens) the flow and its writer. Throws on failure.
        VideoWriter(Domain& domain, VideoFlowParams const& params, int commitBatchHint);
        ~VideoWriter();

        VideoWriter(VideoWriter const&) = delete;
        VideoWriter& operator=(VideoWriter const&) = delete;

        /// Copies one full v210 frame into the grain at `grainIndex` and
        /// commits it. `frameBytes` must equal grainSize (single-plane v210)
        /// or the fill-plane size for v210a. Returns MXL status.
        mxlStatus writeFrame(std::uint64_t grainIndex, std::uint8_t const* frameData, std::size_t frameBytes);

        /// Writes fill+key planes for video/v210a flows.
        mxlStatus writeFrameWithKey(std::uint64_t grainIndex, std::uint8_t const* fill, std::size_t fillBytes, std::uint8_t const* key,
            std::size_t keyBytes);

        /// Commits an invalid grain (§3.5: signal-loss gap marking).
        mxlStatus writeInvalid(std::uint64_t grainIndex);

        [[nodiscard]] mxlRational grainRate() const
        {
            return _configInfo.common.grainRate;
        }

        [[nodiscard]] std::uint32_t actualGrainCount() const
        {
            return _configInfo.discrete.grainCount;
        }

        [[nodiscard]] std::size_t grainPayloadSize() const
        {
            return _grainPayloadSize;
        }

        [[nodiscard]] std::string const& flowId() const
        {
            return _flowIdString;
        }

    private:
        Domain& _domain;
        mxlFlowWriter _writer = nullptr;
        mxlFlowConfigInfo _configInfo{};
        std::size_t _grainPayloadSize = 0;
        std::string _flowIdString;
    };
}
