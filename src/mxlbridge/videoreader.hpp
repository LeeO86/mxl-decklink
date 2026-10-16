// SPDX-License-Identifier: MIT
// Video grain reading for the output path (SPECIFICATION.md §2.4).
//
// An interlaced flow (one field per grain at twice the frame rate,
// util/interlace.hpp) is read as frames: every index, rate, head and count of
// this class is then a frame (k), and getGrain(k) weaves the field grains 2k
// and 2k + 1 into one frame. A progressive flow is read grain by grain.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

        /// Blocking fetch with timeout (CHx_READER_TIMEOUT_MS). Interlaced:
        /// frame `index`, both fields within the timeout, woven into a buffer
        /// of this reader (valid until the next call); a frame with an
        /// invalid field is returned invalid (flag set, no payload).
        mxlStatus getGrain(std::uint64_t index, std::uint64_t timeoutNs, Grain& out);

        /// Frames per second (interlaced: half MXL's field grain rate).
        [[nodiscard]] mxlRational grainRate() const
        {
            return _frameRate;
        }

        /// Frames the ring holds.
        [[nodiscard]] std::uint32_t grainCount() const
        {
            return _interlaced ? _configInfo.discrete.grainCount / 2 : _configInfo.discrete.grainCount;
        }

        /// Newest frame of the flow ring (interlaced: with both fields), for
        /// lag metrics §7.3 and the read position.
        [[nodiscard]] std::uint64_t headIndex() const;

        /// TAI time at which frame `index` is complete for a sync group wait:
        /// the time of its last grain (interlaced: the second field).
        [[nodiscard]] std::uint64_t syncTimestamp(std::uint64_t index) const;

        /// interlace_mode of the flow definition is interlaced_tff or _bff.
        [[nodiscard]] bool interlaced() const
        {
            return _interlaced;
        }

        [[nodiscard]] bool bottomFieldFirst() const
        {
            return _bottomFieldFirst;
        }

        /// True when MXL reports that the writer re-created the flow (its
        /// data file changed): a non-blocking read past the head returns
        /// MXL_ERR_FLOW_INVALID. Does not wait.
        [[nodiscard]] bool flowInvalid() const;

        [[nodiscard]] mxlFlowConfigInfo const& configInfo() const
        {
            return _configInfo;
        }

        [[nodiscard]] std::string const& flowId() const
        {
            return _flowId;
        }

        [[nodiscard]] mxlFlowReader handle() const
        {
            return _reader;
        }

    private:
        Domain& _domain;
        std::string _flowId;
        mxlFlowReader _reader = nullptr;
        mxlFlowConfigInfo _configInfo{};
        mxlRational _frameRate{0, 1};
        bool _interlaced = false;
        bool _bottomFieldFirst = false;
        std::uint32_t _frameHeight = 0;
        std::vector<std::uint8_t> _woven; // interlaced: the frame getGrain returns
    };
}
