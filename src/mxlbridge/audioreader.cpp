// SPDX-License-Identifier: MIT
#include "audioreader.hpp"

#include <stdexcept>

#include "util/audioconv.hpp"

namespace mxldl::mxlbridge
{
    AudioReader::AudioReader(Domain& domain, std::string flowId)
        : _domain(domain)
        , _flowId(std::move(flowId))
    {
        auto const status = ::mxlCreateFlowReader(domain.instance(), _flowId.c_str(), nullptr, &_reader);
        if (status != MXL_STATUS_OK)
        {
            throw std::runtime_error("mxlCreateFlowReader (audio " + _flowId + ") failed with status " + std::to_string(status));
        }
        if (::mxlFlowReaderGetConfigInfo(_reader, &_configInfo) != MXL_STATUS_OK)
        {
            ::mxlReleaseFlowReader(domain.instance(), _reader);
            _reader = nullptr;
            throw std::runtime_error("mxlFlowReaderGetConfigInfo (audio " + _flowId + ") failed");
        }
    }

    AudioReader::~AudioReader()
    {
        if (_reader != nullptr)
        {
            ::mxlReleaseFlowReader(_domain.instance(), _reader);
        }
    }

    mxlStatus AudioReader::readSamples(std::uint64_t endIndex, std::size_t sampleFrames, std::uint64_t timeoutNs, void* dst,
        std::size_t deckLinkChannels, config::AudioSampleType sampleType)
    {
        mxlWrappedMultiBufferSlice slices{};
        auto const status = ::mxlFlowReaderGetSamples(_reader, endIndex, sampleFrames, timeoutNs, &slices);
        if (status != MXL_STATUS_OK)
        {
            return status;
        }
        if (sampleType == config::AudioSampleType::Int32)
        {
            util::interleaveFloatToInt32(slices, sampleFrames, deckLinkChannels, static_cast<std::int32_t*>(dst));
        }
        else
        {
            util::interleaveFloatToInt16(slices, sampleFrames, deckLinkChannels, static_cast<std::int16_t*>(dst));
        }
        return MXL_STATUS_OK;
    }
}
