// SPDX-License-Identifier: MIT
#include "videomodes.hpp"

#include <DeckLinkAPI.h>

namespace mxldl::config
{
    namespace
    {
        std::vector<VideoMode> buildTable()
        {
            // §3.2.1 plus the remaining broadcast-relevant SD/HD/UHD/8K modes
            // that map 1:1 onto BMDDisplayMode values. DCI/PC modes are
            // intentionally absent (not representable in MXL v1.0 flows with
            // standard broadcast rates).
            return {
                // SD
                {"PAL", bmdModePAL, 720, 576, 25, 1, true},
                {"NTSC", bmdModeNTSC, 720, 486, 30000, 1001, true},

                // HD 720p
                {"HD720p50", bmdModeHD720p50, 1280, 720, 50, 1, false},
                {"HD720p5994", bmdModeHD720p5994, 1280, 720, 60000, 1001, false},
                {"HD720p60", bmdModeHD720p60, 1280, 720, 60, 1, false},

                // HD 1080 interlaced
                {"HD1080i50", bmdModeHD1080i50, 1920, 1080, 25, 1, true},
                {"HD1080i5994", bmdModeHD1080i5994, 1920, 1080, 30000, 1001, true},

                // HD 1080 progressive
                {"HD1080p2398", bmdModeHD1080p2398, 1920, 1080, 24000, 1001, false},
                {"HD1080p24", bmdModeHD1080p24, 1920, 1080, 24, 1, false},
                {"HD1080p25", bmdModeHD1080p25, 1920, 1080, 25, 1, false},
                {"HD1080p2997", bmdModeHD1080p2997, 1920, 1080, 30000, 1001, false},
                {"HD1080p30", bmdModeHD1080p30, 1920, 1080, 30, 1, false},
                {"HD1080p50", bmdModeHD1080p50, 1920, 1080, 50, 1, false},
                {"HD1080p5994", bmdModeHD1080p5994, 1920, 1080, 60000, 1001, false},
                {"HD1080p60", bmdModeHD1080p6000, 1920, 1080, 60, 1, false},

                // UHD 2160
                {"4K2160p2398", bmdMode4K2160p2398, 3840, 2160, 24000, 1001, false},
                {"4K2160p24", bmdMode4K2160p24, 3840, 2160, 24, 1, false},
                {"4K2160p25", bmdMode4K2160p25, 3840, 2160, 25, 1, false},
                {"4K2160p2997", bmdMode4K2160p2997, 3840, 2160, 30000, 1001, false},
                {"4K2160p30", bmdMode4K2160p30, 3840, 2160, 30, 1, false},
                {"4K2160p50", bmdMode4K2160p50, 3840, 2160, 50, 1, false},
                {"4K2160p5994", bmdMode4K2160p5994, 3840, 2160, 60000, 1001, false},
                {"4K2160p60", bmdMode4K2160p60, 3840, 2160, 60, 1, false},

                // 8K 4320
                {"8K4320p25", bmdMode8K4320p25, 7680, 4320, 25, 1, false},
                {"8K4320p2997", bmdMode8K4320p2997, 7680, 4320, 30000, 1001, false},
                {"8K4320p30", bmdMode8K4320p30, 7680, 4320, 30, 1, false},
                {"8K4320p50", bmdMode8K4320p50, 7680, 4320, 50, 1, false},
                {"8K4320p5994", bmdMode8K4320p5994, 7680, 4320, 60000, 1001, false},
                {"8K4320p60", bmdMode8K4320p60, 7680, 4320, 60, 1, false},
            };
        }
    }

    std::vector<VideoMode> const& allVideoModes()
    {
        static std::vector<VideoMode> const table = buildTable();
        return table;
    }

    std::optional<VideoMode> lookupVideoMode(std::string_view name)
    {
        for (auto const& m : allVideoModes())
        {
            if (m.name == name)
            {
                return m;
            }
        }
        return std::nullopt;
    }

    std::optional<VideoMode> lookupVideoModeByBmd(std::uint32_t bmdDisplayMode)
    {
        for (auto const& m : allVideoModes())
        {
            if (m.bmdDisplayMode == bmdDisplayMode)
            {
                return m;
            }
        }
        return std::nullopt;
    }
}
