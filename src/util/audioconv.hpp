// SPDX-License-Identifier: MIT
// PCM interleaved integer → deinterleaved float32 per SPECIFICATION.md §3.4,
// writing directly into MXL wrapped multi-buffer slices (§2.3). Supports a
// DeckLink↔MXL channel routing matrix (CHx_AFn_MAP).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <mxl/flow.h>

namespace mxldl::util
{
    /// 1:1 deinterleave (DeckLink channel i → MXL buffer i).
    void deinterleaveInt32ToFloat(std::int32_t const* src, std::size_t sampleFrames, std::size_t channelCount,
        mxlMutableWrappedMultiBufferSlice const& dst);

    void deinterleaveInt16ToFloat(std::int16_t const* src, std::size_t sampleFrames, std::size_t channelCount,
        mxlMutableWrappedMultiBufferSlice const& dst);

    /// Matrix deinterleave: MXL buffer f ← DeckLink channel map[f].
    /// `deckLinkChannels` is the interleaved width of `src`; `map.size()` is
    /// the MXL flow channel count.
    void deinterleaveInt32ToFloatMapped(std::int32_t const* src, std::size_t sampleFrames, std::size_t deckLinkChannels, std::span<int const> map,
        mxlMutableWrappedMultiBufferSlice const& dst);

    void deinterleaveInt16ToFloatMapped(std::int16_t const* src, std::size_t sampleFrames, std::size_t deckLinkChannels, std::span<int const> map,
        mxlMutableWrappedMultiBufferSlice const& dst);

    /// 1:1 interleave (MXL buffer i → DeckLink channel i); missing buffers → 0.
    void interleaveFloatToInt32(mxlWrappedMultiBufferSlice const& src, std::size_t sampleFrames, std::size_t channelCount, std::int32_t* dst);

    void interleaveFloatToInt16(mxlWrappedMultiBufferSlice const& src, std::size_t sampleFrames, std::size_t channelCount, std::int16_t* dst);

    /// Matrix interleave into an existing DeckLink PCM buffer: for each flow
    /// channel f, write MXL buffer f into DeckLink channel map[f]. Does not
    /// clear unmapped DeckLink channels (caller zeros the buffer first).
    void interleaveFloatToInt32Mapped(mxlWrappedMultiBufferSlice const& src, std::size_t sampleFrames, std::span<int const> map,
        std::size_t deckLinkChannels, std::int32_t* dst);

    void interleaveFloatToInt16Mapped(mxlWrappedMultiBufferSlice const& src, std::size_t sampleFrames, std::span<int const> map,
        std::size_t deckLinkChannels, std::int16_t* dst);
}
