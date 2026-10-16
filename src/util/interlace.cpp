// SPDX-License-Identifier: MIT
#include "interlace.hpp"

#include <algorithm>
#include <cstring>

#include <mxl/time.h>

namespace mxldl::util
{
    std::uint64_t firstFieldGrainIndex(mxlRational const& frameRate, std::uint64_t arrivalTai)
    {
        auto const periodNs = 1'000'000'000ULL * static_cast<std::uint64_t>(frameRate.denominator) / static_cast<std::uint64_t>(frameRate.numerator);
        return 2 * ::mxlTimestampToIndex(&frameRate, arrivalTai > periodNs ? arrivalTai - periodNs : 0);
    }

    void extractField(std::uint8_t const* frame, std::size_t frameRowBytes, std::uint32_t frameHeight, unsigned parity, std::uint8_t* field,
        std::size_t fieldRowBytes)
    {
        auto const n = std::min(frameRowBytes, fieldRowBytes);
        for (std::uint32_t r = 0; r < frameHeight / 2; ++r)
        {
            std::memcpy(field + r * fieldRowBytes, frame + (2 * static_cast<std::size_t>(r) + parity) * frameRowBytes, n);
        }
    }

    void weaveField(std::uint8_t const* field, std::size_t fieldRowBytes, std::uint32_t frameHeight, unsigned parity, std::uint8_t* frame,
        std::size_t frameRowBytes)
    {
        auto const n = std::min(frameRowBytes, fieldRowBytes);
        for (std::uint32_t r = 0; r < frameHeight / 2; ++r)
        {
            std::memcpy(frame + (2 * static_cast<std::size_t>(r) + parity) * frameRowBytes, field + r * fieldRowBytes, n);
        }
    }

    bool ancLineInSecondField(std::uint32_t frameHeight, std::uint32_t line)
    {
        switch (frameHeight)
        {
            case 1080: return line >= 564;
            case 576: return line >= 313;
            case 486: return line >= 266 || line <= 3;
            default: return false;
        }
    }
}
