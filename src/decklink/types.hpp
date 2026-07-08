// SPDX-License-Identifier: MIT
// Backend-neutral value types for the DeckLink abstraction (SPECIFICATION.md
// §2.2 "Streaming" / "Device Manager" modules). The abstraction exists so a
// deterministic mock backend can drive the identical channel logic in tests
// (IMPLEMENTATION_PLAN.md §4).
#pragma once

#include <cstdint>
#include <functional>
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
}
