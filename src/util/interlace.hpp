// SPDX-License-Identifier: MIT
// Interlaced video in MXL (SPECIFICATION.md §2.3, §2.4). Shared convention of
// the media functions: an interlaced flow (interlace_mode interlaced_tff or
// interlaced_bff, grain_rate = the frame rate in the flow definition) holds
// one field per grain (height / 2 lines) at twice the frame rate. Grain 2k is
// the first field in time of TAI frame k, grain 2k + 1 the second. tff: the
// first field is the top field (even frame lines 0, 2, 4, ...); bff: the
// bottom field (odd lines). A reader weaves grain i into the frame lines of
// parity (i & 1) ^ bff. ANC of an interlaced video runs at the field rate,
// with the same index as the video field.
#pragma once

#include <cstddef>
#include <cstdint>

#include <mxl/rational.h>

namespace mxldl::util
{
    /// Frame line parity (0 = even lines, 1 = odd lines) of the field in grain
    /// `grainIndex` of an interlaced flow.
    constexpr unsigned fieldParity(std::uint64_t grainIndex, bool bottomFieldFirst)
    {
        return static_cast<unsigned>(grainIndex & 1U) ^ (bottomFieldFirst ? 1U : 0U);
    }

    /// Frame k of an interlaced flow whose newest grain (field) is
    /// `fieldHead`: the newest frame with both fields written. 0 stays 0 (no
    /// grain yet).
    constexpr std::uint64_t frameHeadOfFieldHead(std::uint64_t fieldHead)
    {
        return fieldHead == 0 ? 0 : (fieldHead - 1) / 2;
    }

    /// Capture: grain index 2k of the first field of the frame that arrived
    /// at TAI `arrivalTai`. k is the TAI frame index (at the frame rate) of
    /// the frame's start, one frame period before it arrived.
    std::uint64_t firstFieldGrainIndex(mxlRational const& frameRate, std::uint64_t arrivalTai);

    /// Copies the frame lines of `parity` (parity, parity + 2, ...) into
    /// consecutive rows of `field` (frameHeight / 2 rows). Each row copies
    /// min(frameRowBytes, fieldRowBytes) bytes.
    void extractField(std::uint8_t const* frame, std::size_t frameRowBytes, std::uint32_t frameHeight, unsigned parity, std::uint8_t* field,
        std::size_t fieldRowBytes);

    /// The inverse: the frameHeight / 2 rows of `field` into the frame lines
    /// of `parity`.
    void weaveField(std::uint8_t const* field, std::size_t fieldRowBytes, std::uint32_t frameHeight, unsigned parity, std::uint8_t* frame,
        std::size_t frameRowBytes);

    /// True when SDI line `line` (SMPTE line number, as IDeckLinkAncillary
    /// Packet::GetLineNumber reports it) of an interlaced mode with
    /// `frameHeight` active lines lies in the second field in time: 1080i
    /// (SMPTE ST 274) lines 564-1125, 625-line PAL (ITU-R BT.656) lines
    /// 313-625, 525-line NTSC lines 266-525 and 1-3. Other heights: false.
    bool ancLineInSecondField(std::uint32_t frameHeight, std::uint32_t line);
}
