// SPDX-License-Identifier: MIT
// Audio sample-batch writing (SPECIFICATION.md §2.3/§3.4).
#pragma once

#include <cstdint>
#include <string>

#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/rational.h>

#include "config/config.hpp"
#include "mxlbridge/domain.hpp"
#include "mxlbridge/flowdef.hpp"

namespace mxldl::mxlbridge
{
    class AudioWriter
    {
    public:
        AudioWriter(Domain& domain, AudioFlowParams const& params, int commitBatchHint);
        ~AudioWriter();

        AudioWriter(AudioWriter const&) = delete;
        AudioWriter& operator=(AudioWriter const&) = delete;

        /// Converts one interleaved integer PCM packet to deinterleaved
        /// float32 and commits it so that the batch ENDS at sample index
        /// `endIndex` (MXL sample-range convention: `count` samples up to
        /// `index`). `endIndex` is derived from the packet's TAI timestamp:
        ///   endIndex = mxlTimestampToIndex(48000/1, taiOfFirstSample) + frames
        mxlStatus writeSamples(std::uint64_t endIndex, void const* interleavedPcm, std::size_t sampleFrames, std::size_t deckLinkChannels,
            config::AudioSampleType sampleType);

        [[nodiscard]] mxlRational sampleRate() const
        {
            return _configInfo.common.grainRate;
        }

        [[nodiscard]] std::uint32_t channelCount() const
        {
            return _configInfo.continuous.channelCount;
        }

        [[nodiscard]] std::uint32_t bufferLength() const
        {
            return _configInfo.continuous.bufferLength;
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
