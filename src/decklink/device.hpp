// SPDX-License-Identifier: MIT
// Abstract DeckLink device interfaces implemented by the SDK backend and the
// mock backend (IMPLEMENTATION_PLAN.md §4).
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "config/config.hpp"
#include "decklink/types.hpp"

namespace mxldl::dl
{
    /// One capture stream on a sub-device (wraps IDeckLinkInput).
    class ICaptureSession
    {
    public:
        virtual ~ICaptureSession() = default;

        struct Params
        {
            config::VideoMode mode;
            bool enableFormatDetection = false;
            config::PixelFormat pixelFormat = config::PixelFormat::YUV10;
            bool audioEnable = true;
            int audioChannelCount = 16;
            config::AudioSampleType audioSampleType = config::AudioSampleType::Int32;
            bool collectAncPackets = false;
        };

        /// EnableVideoInput/EnableAudioInput + SetCallback. Error string on failure.
        virtual std::optional<std::string> enable(Params const& params, CaptureCallbacks callbacks) = 0;
        virtual std::optional<std::string> start() = 0;
        virtual void stop() = 0;
        /// PauseStreams(); FlushStreams(); StartStreams() reset idiom (§3.6).
        virtual void resetStreams() = 0;
        /// Re-enable with a new mode after auto-detect (§3.2).
        virtual std::optional<std::string> changeMode(config::VideoMode const& mode) = 0;
        virtual void disable() = 0;

        /// bmdDeckLinkStatusVideoInputSignalLocked (§3.6 housekeeping).
        virtual bool signalLocked() = 0;
        /// Hardware reference clock "now" in ns, if available (§3.5).
        virtual std::optional<std::uint64_t> hardwareClockNowNs() = 0;
    };

    /// One playback stream on a sub-device (wraps IDeckLinkOutput).
    class IPlaybackSession
    {
    public:
        virtual ~IPlaybackSession() = default;

        struct Params
        {
            config::VideoMode mode;
            config::PixelFormat pixelFormat = config::PixelFormat::YUV10;
            bool audioEnable = true;
            int audioChannelCount = 16;
            config::AudioSampleType audioSampleType = config::AudioSampleType::Int32;
        };

        virtual std::optional<std::string> enable(Params const& params, PlaybackCallbacks callbacks) = 0;

        /// Copies `bytes` into a device frame and schedules it at stream time
        /// `displayTime` (units of `timeScale`). Returns a frame id used in
        /// onFrameCompleted, or nullopt on error.
        virtual std::optional<std::uint64_t> scheduleFrame(std::uint8_t const* bytes, std::size_t size, std::int64_t displayTime,
            std::int64_t displayDuration, std::int64_t timeScale) = 0;

        /// ScheduleAudioSamples with interleaved PCM.
        virtual std::optional<std::string> scheduleAudio(void const* interleavedPcm, std::uint32_t sampleFrames) = 0;

        virtual std::optional<std::string> beginAudioPreroll() = 0;
        virtual std::optional<std::string> endAudioPreroll() = 0;
        virtual std::optional<std::string> startPlayback(std::int64_t startTime, std::int64_t timeScale) = 0;
        virtual void stopPlayback() = 0;
        virtual void disable() = 0;

        virtual std::uint32_t bufferedVideoFrames() = 0;
        virtual std::uint32_t bufferedAudioFrames() = 0;
        /// Reference lock status (genlock / PTP on IP cards, §3.6).
        virtual bool referenceLocked() = 0;
    };

    /// One sub-device of the owned card.
    class ISubDevice
    {
    public:
        virtual ~ISubDevice() = default;

        [[nodiscard]] virtual SubDeviceInfo const& info() const = 0;
        [[nodiscard]] virtual bool supportsMode(config::VideoMode const& mode, config::Direction direction) = 0;
        virtual std::unique_ptr<ICaptureSession> openCapture() = 0;
        virtual std::unique_ptr<IPlaybackSession> openPlayback() = 0;
    };

    /// Events raised by the card outside stream callbacks.
    struct CardCallbacks
    {
        /// External profile change (§3.9): the process must exit with code 2.
        std::function<void()> onExternalProfileChange;
    };

    /// The owned physical card: the set of its sub-devices plus profile
    /// management (§3.1, §3.9).
    class ICard
    {
    public:
        virtual ~ICard() = default;

        [[nodiscard]] virtual std::string const& displayName() const = 0;
        [[nodiscard]] virtual std::uint32_t persistentId() const = 0;
        [[nodiscard]] virtual std::size_t subDeviceCount() const = 0;
        virtual ISubDevice& subDevice(std::size_t index) = 0;

        /// Applies MXL_DECKLINK_CARD_PROFILE. No-op (with log) on
        /// profile-less cards. Error string on failure.
        virtual std::optional<std::string> applyProfile(config::CardProfile profile) = 0;
        virtual void setCallbacks(CardCallbacks callbacks) = 0;
    };

    /// Enumeration + card matching (§3.1). Implemented by both backends.
    class IBackend
    {
    public:
        virtual ~IBackend() = default;

        struct CardSelector
        {
            std::optional<std::uint32_t> persistentId;
            std::optional<std::string> displayName;
            std::optional<int> index;
        };

        /// Enumerates and returns the matched card, or an error string.
        virtual std::variant<std::unique_ptr<ICard>, std::string> openCard(CardSelector const& selector) = 0;
    };

    std::unique_ptr<IBackend> makeSdkBackend();
    std::unique_ptr<IBackend> makeMockBackend(config::EnvReader const& env);
}
