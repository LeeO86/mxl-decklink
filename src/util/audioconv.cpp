// SPDX-License-Identifier: MIT
#include "audioconv.hpp"

#include <cmath>
#include <limits>

namespace mxldl::util
{
    namespace
    {
        // §2.3: float32 = int32 / 2147483648.0f; the int16 variant scales by 2^15.
        constexpr float kInt32Scale = 1.0f / 2147483648.0f;
        constexpr float kInt16Scale = 1.0f / 32768.0f;

        template<typename SampleT>
        void deinterleaveToFloat(SampleT const* src, std::size_t sampleFrames, std::size_t channelCount,
            mxlMutableWrappedMultiBufferSlice const& dst, float scale)
        {
            for (std::size_t chan = 0; chan < channelCount && chan < dst.count; ++chan)
            {
                std::size_t frame = 0;
                for (auto const& fragment : dst.base.fragments)
                {
                    if (fragment.size == 0)
                    {
                        continue;
                    }
                    auto* out = reinterpret_cast<float*>(static_cast<std::uint8_t*>(fragment.pointer) + chan * dst.stride);
                    std::size_t const fragSamples = fragment.size / sizeof(float);
                    for (std::size_t i = 0; i < fragSamples && frame < sampleFrames; ++i, ++frame)
                    {
                        out[i] = static_cast<float>(src[frame * channelCount + chan]) * scale;
                    }
                }
            }
        }

        template<typename SampleT>
        SampleT clampedFromFloat(float scaled)
        {
            // Compare against the exact float renditions of the type bounds;
            // note float(int32 max) rounds up to 2^31, hence >= for the clamp.
            if (scaled >= static_cast<float>(std::numeric_limits<SampleT>::max()))
            {
                return std::numeric_limits<SampleT>::max();
            }
            if (scaled <= static_cast<float>(std::numeric_limits<SampleT>::min()))
            {
                return std::numeric_limits<SampleT>::min();
            }
            return static_cast<SampleT>(std::lrintf(scaled));
        }

        template<typename SampleT>
        void interleaveFromFloat(mxlWrappedMultiBufferSlice const& src, std::size_t sampleFrames, std::size_t channelCount, SampleT* dst, float scale)
        {
            for (std::size_t chan = 0; chan < channelCount; ++chan)
            {
                if (chan >= src.count)
                {
                    for (std::size_t frame = 0; frame < sampleFrames; ++frame)
                    {
                        dst[frame * channelCount + chan] = 0;
                    }
                    continue;
                }
                std::size_t frame = 0;
                for (auto const& fragment : src.base.fragments)
                {
                    if (fragment.size == 0)
                    {
                        continue;
                    }
                    auto const* in = reinterpret_cast<float const*>(static_cast<std::uint8_t const*>(fragment.pointer) + chan * src.stride);
                    std::size_t const fragSamples = fragment.size / sizeof(float);
                    for (std::size_t i = 0; i < fragSamples && frame < sampleFrames; ++i, ++frame)
                    {
                        dst[frame * channelCount + chan] = clampedFromFloat<SampleT>(in[i] * scale);
                    }
                }
            }
        }
    }

    void deinterleaveInt32ToFloat(std::int32_t const* src, std::size_t sampleFrames, std::size_t channelCount,
        mxlMutableWrappedMultiBufferSlice const& dst)
    {
        deinterleaveToFloat(src, sampleFrames, channelCount, dst, kInt32Scale);
    }

    void deinterleaveInt16ToFloat(std::int16_t const* src, std::size_t sampleFrames, std::size_t channelCount,
        mxlMutableWrappedMultiBufferSlice const& dst)
    {
        deinterleaveToFloat(src, sampleFrames, channelCount, dst, kInt16Scale);
    }

    void interleaveFloatToInt32(mxlWrappedMultiBufferSlice const& src, std::size_t sampleFrames, std::size_t channelCount, std::int32_t* dst)
    {
        interleaveFromFloat(src, sampleFrames, channelCount, dst, 2147483648.0f);
    }

    void interleaveFloatToInt16(mxlWrappedMultiBufferSlice const& src, std::size_t sampleFrames, std::size_t channelCount, std::int16_t* dst)
    {
        interleaveFromFloat(src, sampleFrames, channelCount, dst, 32768.0f);
    }
}
