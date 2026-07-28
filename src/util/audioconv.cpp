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
        void deinterleaveToFloatMapped(SampleT const* src, std::size_t sampleFrames, std::size_t deckLinkChannels, std::span<int const> map,
            mxlMutableWrappedMultiBufferSlice const& dst, float scale)
        {
            for (std::size_t flowChan = 0; flowChan < map.size() && flowChan < dst.count; ++flowChan)
            {
                int const dlChan = map[flowChan];
                if (dlChan < 0 || static_cast<std::size_t>(dlChan) >= deckLinkChannels)
                {
                    continue;
                }
                std::size_t frame = 0;
                for (auto const& fragment : dst.base.fragments)
                {
                    if (fragment.size == 0)
                    {
                        continue;
                    }
                    auto* out = reinterpret_cast<float*>(static_cast<std::uint8_t*>(fragment.pointer) + flowChan * dst.stride);
                    std::size_t const fragSamples = fragment.size / sizeof(float);
                    for (std::size_t i = 0; i < fragSamples && frame < sampleFrames; ++i, ++frame)
                    {
                        out[i] = static_cast<float>(src[frame * deckLinkChannels + static_cast<std::size_t>(dlChan)]) * scale;
                    }
                }
            }
        }

        template<typename SampleT>
        SampleT clampedFromFloat(float scaled)
        {
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
        void interleaveFromFloatMapped(mxlWrappedMultiBufferSlice const& src, std::size_t sampleFrames, std::span<int const> map,
            std::size_t deckLinkChannels, SampleT* dst, float scale)
        {
            for (std::size_t flowChan = 0; flowChan < map.size(); ++flowChan)
            {
                int const dlChan = map[flowChan];
                if (dlChan < 0 || static_cast<std::size_t>(dlChan) >= deckLinkChannels)
                {
                    continue;
                }
                if (flowChan >= src.count)
                {
                    for (std::size_t frame = 0; frame < sampleFrames; ++frame)
                    {
                        dst[frame * deckLinkChannels + static_cast<std::size_t>(dlChan)] = 0;
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
                    auto const* in = reinterpret_cast<float const*>(static_cast<std::uint8_t const*>(fragment.pointer) + flowChan * src.stride);
                    std::size_t const fragSamples = fragment.size / sizeof(float);
                    for (std::size_t i = 0; i < fragSamples && frame < sampleFrames; ++i, ++frame)
                    {
                        dst[frame * deckLinkChannels + static_cast<std::size_t>(dlChan)] = clampedFromFloat<SampleT>(in[i] * scale);
                    }
                }
            }
        }
    }

    void deinterleaveInt32ToFloat(std::int32_t const* src, std::size_t sampleFrames, std::size_t channelCount,
        mxlMutableWrappedMultiBufferSlice const& dst)
    {
        // Identity map 0..channelCount-1.
        // Allocate a small stack map for the common DeckLink widths.
        int identity[64];
        std::size_t const n = channelCount < 64 ? channelCount : 64;
        for (std::size_t i = 0; i < n; ++i)
        {
            identity[i] = static_cast<int>(i);
        }
        deinterleaveInt32ToFloatMapped(src, sampleFrames, channelCount, std::span<int const>{identity, n}, dst);
    }

    void deinterleaveInt16ToFloat(std::int16_t const* src, std::size_t sampleFrames, std::size_t channelCount,
        mxlMutableWrappedMultiBufferSlice const& dst)
    {
        int identity[64];
        std::size_t const n = channelCount < 64 ? channelCount : 64;
        for (std::size_t i = 0; i < n; ++i)
        {
            identity[i] = static_cast<int>(i);
        }
        deinterleaveInt16ToFloatMapped(src, sampleFrames, channelCount, std::span<int const>{identity, n}, dst);
    }

    void deinterleaveInt32ToFloatMapped(std::int32_t const* src, std::size_t sampleFrames, std::size_t deckLinkChannels, std::span<int const> map,
        mxlMutableWrappedMultiBufferSlice const& dst)
    {
        deinterleaveToFloatMapped(src, sampleFrames, deckLinkChannels, map, dst, kInt32Scale);
    }

    void deinterleaveInt16ToFloatMapped(std::int16_t const* src, std::size_t sampleFrames, std::size_t deckLinkChannels, std::span<int const> map,
        mxlMutableWrappedMultiBufferSlice const& dst)
    {
        deinterleaveToFloatMapped(src, sampleFrames, deckLinkChannels, map, dst, kInt16Scale);
    }

    void interleaveFloatToInt32(mxlWrappedMultiBufferSlice const& src, std::size_t sampleFrames, std::size_t channelCount, std::int32_t* dst)
    {
        int identity[64];
        std::size_t const n = channelCount < 64 ? channelCount : 64;
        for (std::size_t i = 0; i < n; ++i)
        {
            identity[i] = static_cast<int>(i);
        }
        // Clear then map so unmapped (beyond src.count) stays zero via identity path.
        for (std::size_t frame = 0; frame < sampleFrames; ++frame)
        {
            for (std::size_t ch = 0; ch < channelCount; ++ch)
            {
                dst[frame * channelCount + ch] = 0;
            }
        }
        interleaveFloatToInt32Mapped(src, sampleFrames, std::span<int const>{identity, n}, channelCount, dst);
    }

    void interleaveFloatToInt16(mxlWrappedMultiBufferSlice const& src, std::size_t sampleFrames, std::size_t channelCount, std::int16_t* dst)
    {
        int identity[64];
        std::size_t const n = channelCount < 64 ? channelCount : 64;
        for (std::size_t i = 0; i < n; ++i)
        {
            identity[i] = static_cast<int>(i);
        }
        for (std::size_t frame = 0; frame < sampleFrames; ++frame)
        {
            for (std::size_t ch = 0; ch < channelCount; ++ch)
            {
                dst[frame * channelCount + ch] = 0;
            }
        }
        interleaveFloatToInt16Mapped(src, sampleFrames, std::span<int const>{identity, n}, channelCount, dst);
    }

    void interleaveFloatToInt32Mapped(mxlWrappedMultiBufferSlice const& src, std::size_t sampleFrames, std::span<int const> map,
        std::size_t deckLinkChannels, std::int32_t* dst)
    {
        interleaveFromFloatMapped(src, sampleFrames, map, deckLinkChannels, dst, 2147483648.0f);
    }

    void interleaveFloatToInt16Mapped(mxlWrappedMultiBufferSlice const& src, std::size_t sampleFrames, std::span<int const> map,
        std::size_t deckLinkChannels, std::int16_t* dst)
    {
        interleaveFromFloatMapped(src, sampleFrames, map, deckLinkChannels, dst, 32768.0f);
    }
}
