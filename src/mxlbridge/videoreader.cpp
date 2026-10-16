// SPDX-License-Identifier: MIT
#include "videoreader.hpp"

#include <chrono>
#include <limits>
#include <stdexcept>

#include <mxl/time.h>
#include <picojson/picojson.h>

#include "util/interlace.hpp"

namespace mxldl::mxlbridge
{
    VideoReader::VideoReader(Domain& domain, std::string flowId)
        : _domain(domain)
        , _flowId(std::move(flowId))
    {
        auto const status = ::mxlCreateFlowReader(domain.instance(), _flowId.c_str(), nullptr, &_reader);
        if (status != MXL_STATUS_OK)
        {
            throw std::runtime_error("mxlCreateFlowReader (video " + _flowId + ") failed with status " + std::to_string(status));
        }
        if (::mxlFlowReaderGetConfigInfo(_reader, &_configInfo) != MXL_STATUS_OK)
        {
            ::mxlReleaseFlowReader(domain.instance(), _reader);
            _reader = nullptr;
            throw std::runtime_error("mxlFlowReaderGetConfigInfo (video " + _flowId + ") failed");
        }
        _frameRate = _configInfo.common.grainRate;

        // interlace_mode and frame_height are in the flow definition only.
        std::vector<char> def(16384);
        auto size = def.size();
        auto status2 = ::mxlGetFlowDef(domain.instance(), _flowId.c_str(), def.data(), &size);
        if (status2 == MXL_ERR_INVALID_ARG && size > def.size())
        {
            def.resize(size);
            status2 = ::mxlGetFlowDef(domain.instance(), _flowId.c_str(), def.data(), &size);
        }
        picojson::value root;
        if (status2 == MXL_STATUS_OK && picojson::parse(root, std::string(def.data())).empty() && root.is<picojson::object>())
        {
            auto const& obj = root.get<picojson::object>();
            auto const mode = obj.find("interlace_mode");
            auto const height = obj.find("frame_height");
            if (mode != obj.end() && mode->second.is<std::string>() && height != obj.end() && height->second.is<double>())
            {
                auto const& m = mode->second.get<std::string>();
                _interlaced = m == "interlaced_tff" || m == "interlaced_bff";
                _bottomFieldFirst = m == "interlaced_bff";
                _frameHeight = static_cast<std::uint32_t>(height->second.get<double>());
            }
        }
        if (_interlaced)
        {
            // MXL reports the field rate (twice the flow definition's).
            _frameRate.numerator /= 2;
        }
    }

    VideoReader::~VideoReader()
    {
        if (_reader != nullptr)
        {
            ::mxlReleaseFlowReader(_domain.instance(), _reader);
        }
    }

    mxlStatus VideoReader::getGrain(std::uint64_t index, std::uint64_t timeoutNs, Grain& out)
    {
        if (!_interlaced)
        {
            std::uint8_t* payload = nullptr;
            auto const status = ::mxlFlowReaderGetGrain(_reader, index, timeoutNs, &out.info, &payload);
            out.payload = payload;
            return status;
        }

        // Frame `index`: its fields are grains 2k and 2k + 1, both read
        // within the timeout.
        auto const start = std::chrono::steady_clock::now();
        Grain field[2];
        for (std::uint64_t i = 0; i < 2; ++i)
        {
            auto const elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
            std::uint8_t* payload = nullptr;
            auto const status = ::mxlFlowReaderGetGrain(_reader, 2 * index + i, timeoutNs > elapsed ? timeoutNs - elapsed : 0, &field[i].info, &payload);
            field[i].payload = payload;
            if (status != MXL_STATUS_OK)
            {
                return status;
            }
        }
        out.info = field[0].info;
        if (field[0].payload == nullptr || field[1].payload == nullptr || ((field[0].info.flags | field[1].info.flags) & MXL_GRAIN_FLAG_INVALID) != 0)
        {
            out.info.flags |= MXL_GRAIN_FLAG_INVALID;
            out.payload = nullptr;
            return MXL_STATUS_OK;
        }
        // Each field's v210 fill rows into the frame lines of its parity
        // (for v210a the key rows are left out: the output plays the fill).
        std::size_t const lineBytes = _configInfo.discrete.sliceSizes[0];
        _woven.resize(lineBytes * _frameHeight);
        for (std::uint64_t i = 0; i < 2; ++i)
        {
            util::weaveField(field[i].payload, lineBytes, _frameHeight, util::fieldParity(2 * index + i, _bottomFieldFirst), _woven.data(), lineBytes);
        }
        out.payload = _woven.data();
        out.info.grainSize = static_cast<decltype(out.info.grainSize)>(_woven.size());
        return MXL_STATUS_OK;
    }

    std::uint64_t VideoReader::headIndex() const
    {
        mxlFlowRuntimeInfo info{};
        if (::mxlFlowReaderGetRuntimeInfo(_reader, &info) != MXL_STATUS_OK)
        {
            return 0;
        }
        return _interlaced ? util::frameHeadOfFieldHead(info.headIndex) : info.headIndex;
    }

    std::uint64_t VideoReader::syncTimestamp(std::uint64_t index) const
    {
        if (_interlaced)
        {
            return ::mxlIndexToTimestamp(&_configInfo.common.grainRate, 2 * index + 1);
        }
        return ::mxlIndexToTimestamp(&_frameRate, index);
    }

    bool VideoReader::flowInvalid() const
    {
        // Any index past the head is too early; MXL then checks the inode of
        // the flow's data file.
        mxlGrainInfo info{};
        std::uint8_t* payload = nullptr;
        return ::mxlFlowReaderGetGrainNonBlocking(_reader, std::numeric_limits<std::uint64_t>::max() - 1, &info, &payload) == MXL_ERR_FLOW_INVALID;
    }
}
