// SPDX-License-Identifier: MIT
// Audio sample reading for the output path (SPECIFICATION.md §2.4).
#pragma once

#include <cstdint>
#include <string>

#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/rational.h>

#include "config/config.hpp"
#include "mxlbridge/domain.hpp"

namespace mxldl::mxlbridge
{
    class AudioReader
    {
    public:
        AudioReader(Domain& domain, std::string flowId);
        ~AudioReader();

        AudioReader(AudioReader const&) = delete;
        AudioReader& operator=(AudioReader const&) = delete;

        /// Reads `sampleFrames` float32 samples per channel ending at
        /// `endIndex` and interleaves them into integer PCM for the DeckLink
        /// API. `dst` must hold sampleFrames × deckLinkChannels samples.
        mxlStatus readSamples(std::uint64_t endIndex, std::size_t sampleFrames, std::uint64_t timeoutNs, void* dst, std::size_t deckLinkChannels,
            config::AudioSampleType sampleType);

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
            return _flowId;
        }

    private:
        Domain& _domain;
        std::string _flowId;
        mxlFlowReader _reader = nullptr;
        mxlFlowConfigInfo _configInfo{};
    };
}
