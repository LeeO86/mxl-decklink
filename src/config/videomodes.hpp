// SPDX-License-Identifier: MIT
// Video mode table per SPECIFICATION.md §3.2.1.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mxldl::config
{
    struct VideoMode
    {
        std::string name; // CHx_VIDEO_MODE value, e.g. "HD1080p50"
        std::uint32_t bmdDisplayMode = 0; // BMDDisplayMode FourCC value
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::int64_t rateNumerator = 0; // frame rate (frames per second)
        std::int64_t rateDenominator = 1;
        bool interlaced = false;

        [[nodiscard]] double fps() const
        {
            return static_cast<double>(rateNumerator) / static_cast<double>(rateDenominator);
        }
    };

    /// nullopt when the name is not in the table (§3.2.1: reject).
    std::optional<VideoMode> lookupVideoMode(std::string_view name);

    /// Reverse lookup by BMDDisplayMode value (used on auto-detect changes).
    std::optional<VideoMode> lookupVideoModeByBmd(std::uint32_t bmdDisplayMode);

    std::vector<VideoMode> const& allVideoModes();
}
