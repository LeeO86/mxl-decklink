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
        log::info("mxl_audio_flow_writer_created",
            {
                {"flow_id", _flowIdString},
                {"created", created},
                {"channel_count", _configInfo.continuous.channelCount},
                {"buffer_length", _configInfo.continuous.bufferLength},
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
        config::AudioSampleType sampleType)
    {
        // §3.4 SDK requirement: batches must stay below half the ring.
        if (sampleFrames > _configInfo.continuous.bufferLength / 2)
        {
            return MXL_ERR_INVALID_ARG;
        }

        mxlMutableWrappedMultiBufferSlice slices{};
        auto status = ::mxlFlowWriterOpenSamples(_writer, endIndex, sampleFrames, &slices);
        if (status != MXL_STATUS_OK)
        {
            return status;
        }

        if (sampleType == config::AudioSampleType::Int32)
        {
            util::deinterleaveInt32ToFloat(static_cast<std::int32_t const*>(interleavedPcm), sampleFrames, deckLinkChannels, slices);
        }
        else
        {
            util::deinterleaveInt16ToFloat(static_cast<std::int16_t const*>(interleavedPcm), sampleFrames, deckLinkChannels, slices);
        }

        return ::mxlFlowWriterCommitSamples(_writer);
    }
}
