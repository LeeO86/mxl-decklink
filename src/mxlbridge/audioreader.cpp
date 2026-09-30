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
        std::size_t maxRead = 0;
        if (::mxlFlowReaderGetMaxReadLengthSamples(_reader, &maxRead) == MXL_STATUS_OK && maxRead > 0)
        {
            _maxReadLength = maxRead;
        }
        else
        {
            _maxReadLength = _configInfo.continuous.bufferLength / 2;
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
        std::size_t deckLinkChannels, std::span<int const> channelMap, config::AudioSampleType sampleType)
    {
        if (_maxReadLength == 0 || sampleFrames > endIndex)
        {
            return MXL_ERR_INVALID_ARG;
        }

        auto const bytesPerSample = sampleType == config::AudioSampleType::Int32 ? sizeof(std::int32_t) : sizeof(std::int16_t);
        auto* cursor = static_cast<std::uint8_t*>(dst);
        std::uint64_t const start = endIndex - sampleFrames;
        std::size_t remaining = sampleFrames;
        while (remaining > 0)
        {
            auto const count = remaining < _maxReadLength ? remaining : _maxReadLength;
            auto const head = start + (sampleFrames - remaining) + count;
            mxlWrappedMultiBufferSlice slices{};
            auto const status = ::mxlFlowReaderGetSamples(_reader, head, count, timeoutNs, &slices);
            if (status != MXL_STATUS_OK)
            {
                return status;
            }
            if (sampleType == config::AudioSampleType::Int32)
            {
                util::interleaveFloatToInt32Mapped(slices, count, channelMap, deckLinkChannels, reinterpret_cast<std::int32_t*>(cursor));
            }
            else
            {
                util::interleaveFloatToInt16Mapped(slices, count, channelMap, deckLinkChannels, reinterpret_cast<std::int16_t*>(cursor));
            }
            cursor += count * deckLinkChannels * bytesPerSample;
            remaining -= count;
        }
        return MXL_STATUS_OK;
    }
}
