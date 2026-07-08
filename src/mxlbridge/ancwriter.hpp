// SPDX-License-Identifier: MIT
// ANC data grain writing (SPECIFICATION.md §2.3, video/smpte291 flow).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/rational.h>

#include "mxlbridge/domain.hpp"
#include "mxlbridge/flowdef.hpp"
#include "util/anc.hpp"

namespace mxldl::mxlbridge
{
    class AncWriter
    {
    public:
        AncWriter(Domain& domain, AncFlowParams const& params);
        ~AncWriter();

        AncWriter(AncWriter const&) = delete;
        AncWriter& operator=(AncWriter const&) = delete;

        /// Serializes the frame's ANC packets per RFC 8331 §2 and commits the
        /// grain. Empty packet lists commit an empty (but valid) grain so the
        /// data flow stays continuous with video.
        mxlStatus writePackets(std::uint64_t grainIndex, std::vector<util::AncPacket> const& packets);

        [[nodiscard]] mxlRational grainRate() const
        {
            return _configInfo.common.grainRate;
        }

        [[nodiscard]] std::string const& flowId() const
        {
            return _flowIdString;
        }

    private:
        Domain& _domain;
        mxlFlowWriter _writer = nullptr;
        mxlFlowConfigInfo _configInfo{};
        std::string _flowIdString;
    };
}
