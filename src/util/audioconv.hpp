// SPDX-License-Identifier: MIT
// PCM interleaved integer → deinterleaved float32 per SPECIFICATION.md §3.4,
// writing directly into MXL wrapped multi-buffer slices (§2.3).
#pragma once

#include <cstddef>
#include <cstdint>

#include <mxl/flow.h>

namespace mxldl::util
{
    /// Convert `sampleFrames` interleaved 32-bit signed integer samples with
    /// `channelCount` channels into the per-channel ring-buffer fragments of
    /// `dst` (float32, deinterleaved). `dst` must describe `sampleFrames`
    /// samples across at least `channelCount` buffers.
    void deinterleaveInt32ToFloat(std::int32_t const* src, std::size_t sampleFrames, std::size_t channelCount,
        mxlMutableWrappedMultiBufferSlice const& dst);

    /// Same for 16-bit input samples.
    void deinterleaveInt16ToFloat(std::int16_t const* src, std::size_t sampleFrames, std::size_t channelCount,
        mxlMutableWrappedMultiBufferSlice const& dst);

    /// Reverse direction (output path): read deinterleaved float32 fragments
    /// and produce interleaved signed integer PCM for the DeckLink API.
    void interleaveFloatToInt32(mxlWrappedMultiBufferSlice const& src, std::size_t sampleFrames, std::size_t channelCount, std::int32_t* dst);

    void interleaveFloatToInt16(mxlWrappedMultiBufferSlice const& src, std::size_t sampleFrames, std::size_t channelCount, std::int16_t* dst);
}
