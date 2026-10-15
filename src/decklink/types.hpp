// SPDX-License-Identifier: MIT
// Backend-neutral value types for the DeckLink abstraction (SPECIFICATION.md
// §2.2 "Streaming" / "Device Manager" modules). The abstraction exists so a
// deterministic mock backend can drive the identical channel logic in tests
// (IMPLEMENTATION_PLAN.md §4).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "config/videomodes.hpp"
#include "util/anc.hpp"

namespace mxldl::dl
{
    /// A captured video frame as seen inside the input callback (§2.3). The
    /// view is only valid for the duration of the callback.
    struct VideoFrameView
    {
        std::uint8_t const* bytes = nullptr;
        std::size_t rowBytes = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        bool hasNoInputSource = false; // bmdFrameHasNoInputSource
        bool hasHardwareTimestamp = false;
        std::uint64_t hardwareTimestampNs = 0; // nanosecond timescale
        std::vector<util::AncPacket> ancPackets; // filled only when requested
    };

    /// A captured audio packet (§2.3). Interleaved integer PCM at 48 kHz.
    struct AudioPacketView
    {
        void const* bytes = nullptr;
        std::size_t sampleFrames = 0;
    };

    /// Detected format-change notification (§3.2).
    struct FormatChange
    {
        config::VideoMode newMode;
        bool is10Bit = true;
    };

    struct CaptureCallbacks
    {
        /// Called per frame pair. Views are valid only during the call.
        std::function<void(VideoFrameView const&, AudioPacketView const*)> onFrame;
        /// Called on VideoInputFormatChanged (auto-detect only).
        std::function<void(FormatChange const&)> onFormatChanged;
    };

    /// Playback-side completion result (§2.4).
    enum class CompletionResult
    {
        Completed,
        DisplayedLate,
        Dropped,
        Flushed,
    };

    struct PlaybackCallbacks
    {
        /// ScheduledFrameCompleted equivalent; frameId identifies the frame
        /// handed to scheduleFrame().
        std::function<void(std::uint64_t frameId, CompletionResult result)> onFrameCompleted;
        /// RenderAudioSamples equivalent: pull more audio.
        std::function<void(bool preroll)> onRenderAudio;
        std::function<void()> onPlaybackStopped;
    };

    /// Identity of one sub-device of the matched card (§3.1).
    struct SubDeviceInfo
    {
        std::uint32_t persistentId = 0;
        std::int64_t deviceGroupId = 0;
        std::string displayName;
        std::string modelName;
        int subDeviceIndexOnCard = 0; // zero-based within the card
        bool supportsInputFormatDetection = false;
        bool supportsCapture = false;
        bool supportsPlayback = false;
        bool hasProfileManager = false;
    };

    /// How the card signals its SDI output, as configured in the driver
    /// (read-only: this process never sets it; §3.3). Unset when the card
    /// does not report it.
    struct SdiOutputConfig
    {
        /// bmdDeckLinkConfigSMPTELevelAOutput: true = 3G-SDI Level A, false =
        /// Level B (some receivers show 1080p50 Level B as 1080i50).
        std::optional<bool> smpteLevelA;
        std::optional<bool> supportsSmpteLevelA; // BMDDeckLinkSupportsSMPTELevelAOutput
        std::optional<bool> output1080pAsPsF; // bmdDeckLinkConfigOutput1080pAsPsF
        std::optional<std::string> linkConfiguration; // bmdDeckLinkConfigSDIOutputLinkConfiguration: single, dual, quad
        /// CHx_SDI_LEVEL of the output channel (set by the channel, not read
        /// from the card): A, B or driver.
        std::optional<std::string> requestedLevel;

        bool operator==(SdiOutputConfig const&) const = default;
    };

    /// "A", "B" or "unknown".
    inline char const* smpteLevelName(SdiOutputConfig const& c)
    {
        return !c.smpteLevelA ? "unknown" : (*c.smpteLevelA ? "A" : "B");
    }

    /// JSON object for /api/card and /statusz.
    inline std::string sdiOutputJson(SdiOutputConfig const& c)
    {
        auto const flag = [](std::optional<bool> const& v) {
            return v ? std::string(*v ? "true" : "false") : std::string("null");
        };
        std::string out = "{\"smpte_level\":";
        out += c.smpteLevelA ? std::string("\"") + smpteLevelName(c) + "\"" : std::string("null");
        out += ",\"supports_smpte_level_a\":" + flag(c.supportsSmpteLevelA);
        out += ",\"output_1080p_as_psf\":" + flag(c.output1080pAsPsF);
        out += ",\"link_configuration\":" + (c.linkConfiguration ? "\"" + *c.linkConfiguration + "\"" : std::string("null"));
        if (c.requestedLevel)
        {
            out += ",\"requested_level\":\"" + *c.requestedLevel + "\"";
        }
        out += "}";
        return out;
    }

    /// What setting the SDI level did (§7.1 CHx_SDI_LEVEL).
    struct SdiLevelResult
    {
        bool applied = false; // the card took the requested level
        bool fallback = false; // not supported: the driver's setting stays
        bool linkChanged = false; // the SDI output link configuration was set to single link
        std::string details;
    };

    /// Live per-sub-device status from the DeckLink SDK (§7.5 Card tab).
    struct SubDeviceStatus
    {
        std::optional<std::string> detectedInputMode; // bmdDeckLinkStatusDetectedVideoInputMode
        std::optional<std::string> currentInputMode;
        std::optional<std::string> currentOutputMode;
        std::optional<std::string> currentInputPixelFormat;
        std::optional<std::string> lastOutputPixelFormat;
        bool inputSignalLocked = false;
        bool referenceLocked = false;
        bool captureBusy = false;
        bool playbackBusy = false;
        std::optional<std::int64_t> pcieLinkWidth;
        std::optional<std::int64_t> pcieLinkSpeed; // generation
        std::optional<double> temperatureC;
        SdiOutputConfig sdiOutput;
    };
}
