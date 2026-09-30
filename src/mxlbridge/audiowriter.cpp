// SPDX-License-Identifier: MIT
#include "audiowriter.hpp"

#include <stdexcept>

#include "util/audioconv.hpp"
#include "util/logging.hpp"

namespace mxldl::mxlbridge
{
    AudioWriter::AudioWriter(Domain& domain, AudioFlowParams const& params, int commitBatchHint)
        : _domain(domain)
        , _flowIdString(params.id.toString())
    {
        auto const flowDef = buildAudioFlowDef(params);
        auto const options = buildWriterOptions(commitBatchHint);
        bool created = false;
        auto const status = ::mxlCreateFlowWriter(domain.instance(), flowDef.c_str(), options.c_str(), &_writer, &_configInfo, &created);
        if (status != MXL_STATUS_OK)
        {
            throw std::runtime_error("mxlCreateFlowWriter (audio " + _flowIdString + ") failed with status " + std::to_string(status));
        }
        std::size_t maxWrite = 0;
        if (::mxlFlowWriterGetMaxWriteLengthSamples(_writer, &maxWrite) == MXL_STATUS_OK && maxWrite > 0)
        {
            _maxWriteLength = maxWrite;
        }
        else
        {
            _maxWriteLength = _configInfo.continuous.bufferLength / 2;
        }
        log::info("mxl_audio_flow_writer_created",
            {
                {"flow_id", _flowIdString},
                {"created", created},
                {"channel_count", _configInfo.continuous.channelCount},
                {"buffer_length", _configInfo.continuous.bufferLength},
                {"max_write_samples", _maxWriteLength},
            });
    }

    AudioWriter::~AudioWriter()
    {
        if (_writer != nullptr)
        {
            ::mxlReleaseFlowWriter(_domain.instance(), _writer);
        }
    }

    mxlStatus AudioWriter::writeSamples(std::uint64_t endIndex, void const* interleavedPcm, std::size_t sampleFrames, std::size_t deckLinkChannels,
        std::span<int const> channelMap, config::AudioSampleType sampleType)
    {
        if (_maxWriteLength == 0 || sampleFrames > endIndex)
        {
            return MXL_ERR_INVALID_ARG;
        }

        // OpenSamples addresses `count` samples ending at `index`. A DeckLink
        // packet can be larger than one legal write, so split it.
        auto const bytesPerSample = sampleType == config::AudioSampleType::Int32 ? sizeof(std::int32_t) : sizeof(std::int16_t);
        auto const* cursor = static_cast<std::uint8_t const*>(interleavedPcm);
        std::uint64_t const start = endIndex - sampleFrames;
        std::size_t remaining = sampleFrames;
        while (remaining > 0)
        {
            auto const count = remaining < _maxWriteLength ? remaining : _maxWriteLength;
            auto const head = start + (sampleFrames - remaining) + count;
            mxlMutableWrappedMultiBufferSlice slices{};
            auto const status = ::mxlFlowWriterOpenSamples(_writer, head, count, &slices);
            if (status != MXL_STATUS_OK)
            {
                return status;
            }
            if (sampleType == config::AudioSampleType::Int32)
            {
                util::deinterleaveInt32ToFloatMapped(reinterpret_cast<std::int32_t const*>(cursor), count, deckLinkChannels, channelMap, slices);
            }
            else
            {
                util::deinterleaveInt16ToFloatMapped(reinterpret_cast<std::int16_t const*>(cursor), count, deckLinkChannels, channelMap, slices);
            }
            auto const committed = ::mxlFlowWriterCommitSamples(_writer);
            if (committed != MXL_STATUS_OK)
            {
                return committed;
            }
            cursor += count * deckLinkChannels * bytesPerSample;
            remaining -= count;
        }
        return MXL_STATUS_OK;
    }
}
