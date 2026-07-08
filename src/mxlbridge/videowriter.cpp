// SPDX-License-Identifier: MIT
#include "videowriter.hpp"

#include <cstring>
#include <stdexcept>

#include "util/logging.hpp"

namespace mxldl::mxlbridge
{
    VideoWriter::VideoWriter(Domain& domain, VideoFlowParams const& params, int commitBatchHint)
        : _domain(domain)
        , _flowIdString(params.id.toString())
    {
        auto const flowDef = buildVideoFlowDef(params);
        auto const options = buildWriterOptions(commitBatchHint);
        bool created = false;
        auto const status = ::mxlCreateFlowWriter(domain.instance(), flowDef.c_str(), options.c_str(), &_writer, &_configInfo, &created);
        if (status != MXL_STATUS_OK)
        {
            throw std::runtime_error("mxlCreateFlowWriter (video " + _flowIdString + ") failed with status " + std::to_string(status));
        }

        // Payload size: totalSlices × sliceSize summed over planes.
        // mxlGrainInfo carries the authoritative value per grain; cache the
        // config-derived value for validation.
        _grainPayloadSize = 0;
        // grainSize is not part of mxlFlowConfigInfo; derive from slice sizes
        // by probing a grain header.
        mxlGrainInfo probe{};
        if (::mxlFlowWriterGetGrainInfo(_writer, 0, &probe) == MXL_STATUS_OK)
        {
            _grainPayloadSize = probe.grainSize;
        }

        log::info("mxl_video_flow_writer_created",
            {
                {"flow_id", _flowIdString},
                {"created", created},
                {"grain_count", _configInfo.discrete.grainCount},
                {"grain_size", static_cast<std::uint64_t>(_grainPayloadSize)},
            });
    }

    VideoWriter::~VideoWriter()
    {
        if (_writer != nullptr)
        {
            ::mxlReleaseFlowWriter(_domain.instance(), _writer);
        }
    }

    mxlStatus VideoWriter::writeFrame(std::uint64_t grainIndex, std::uint8_t const* frameData, std::size_t frameBytes)
    {
        mxlGrainInfo info{};
        std::uint8_t* payload = nullptr;
        auto status = ::mxlFlowWriterOpenGrain(_writer, grainIndex, &info, &payload);
        if (status != MXL_STATUS_OK)
        {
            return status;
        }
        auto const n = frameBytes < info.grainSize ? frameBytes : static_cast<std::size_t>(info.grainSize);
        std::memcpy(payload, frameData, n);
        info.flags = 0;
        info.validSlices = info.totalSlices;
        status = ::mxlFlowWriterCommitGrain(_writer, &info);
        return status;
    }

    mxlStatus VideoWriter::writeFrameWithKey(std::uint64_t grainIndex, std::uint8_t const* fill, std::size_t fillBytes, std::uint8_t const* key,
        std::size_t keyBytes)
    {
        mxlGrainInfo info{};
        std::uint8_t* payload = nullptr;
        auto status = ::mxlFlowWriterOpenGrain(_writer, grainIndex, &info, &payload);
        if (status != MXL_STATUS_OK)
        {
            return status;
        }
        // v210a layout: fill plane followed by the 10-bit alpha plane.
        std::size_t const total = fillBytes + keyBytes;
        std::size_t const cap = info.grainSize;
        std::size_t const fillCopy = fillBytes < cap ? fillBytes : cap;
        std::memcpy(payload, fill, fillCopy);
        if (total <= cap)
        {
            std::memcpy(payload + fillBytes, key, keyBytes);
        }
        info.flags = 0;
        info.validSlices = info.totalSlices;
        status = ::mxlFlowWriterCommitGrain(_writer, &info);
        return status;
    }

    mxlStatus VideoWriter::writeInvalid(std::uint64_t grainIndex)
    {
        mxlGrainInfo info{};
        std::uint8_t* payload = nullptr;
        auto status = ::mxlFlowWriterOpenGrain(_writer, grainIndex, &info, &payload);
        if (status != MXL_STATUS_OK)
        {
            return status;
        }
        info.flags = MXL_GRAIN_FLAG_INVALID;
        status = ::mxlFlowWriterCommitGrain(_writer, &info);
        return status;
    }
}
