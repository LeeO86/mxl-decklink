// SPDX-License-Identifier: MIT
// NMOS-style MXL flow-descriptor JSON builders (MXL v1.0 flow definition
// format, cf. dmf-mxl/mxl examples/flow-configs).
#pragma once

#include <string>

#include "config/config.hpp"
#include "config/videomodes.hpp"
#include "util/uuid.hpp"

namespace mxldl::mxlbridge
{
    struct VideoFlowParams
    {
        util::Uuid id{};
        std::string label;
        std::string description;
        std::string groupHint; // urn:x-nmos:tag:grouphint/v1.0 value
        std::optional<util::Uuid> sourceId;
        std::optional<util::Uuid> deviceId;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::int64_t rateNumerator = 0;
        std::int64_t rateDenominator = 1;
        bool interlaced = false;
        bool withAlpha = false; // video/v210a
    };

    struct AudioFlowParams
    {
        util::Uuid id{};
        std::string label;
        std::string description;
        std::string groupHint;
        std::optional<util::Uuid> sourceId;
        std::optional<util::Uuid> deviceId;
        std::uint32_t channelCount = 0; // 48 kHz float32 fixed by MXL v1.0
    };

    struct AncFlowParams
    {
        util::Uuid id{};
        std::string label;
        std::string description;
        std::string groupHint;
        std::optional<util::Uuid> sourceId;
        std::optional<util::Uuid> deviceId;
        std::int64_t rateNumerator = 0; // grain rate follows the video channel
        std::int64_t rateDenominator = 1;
    };

    std::string buildVideoFlowDef(VideoFlowParams const& p);
    std::string buildAudioFlowDef(AudioFlowParams const& p);
    std::string buildAncFlowDef(AncFlowParams const& p);

    /// Writer options JSON carrying maxCommitBatchSizeHint (§4.2
    /// CHx_COMMIT_BATCH_HINT → mxlCreateFlowWriter options).
    std::string buildWriterOptions(int commitBatchHint);
}
