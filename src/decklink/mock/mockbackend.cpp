// SPDX-License-Identifier: MIT
// Deterministic software DeckLink card for tests/CI (IMPLEMENTATION_PLAN.md
// §4). Enabled with MXL_DECKLINK_BACKEND=mock. Generates SMPTE-style bars in
// v210 with a frame counter and a 1 kHz tone, paced by CLOCK_TAI; playback
// consumes scheduled frames at rate and reports completions.
//
// Fault injection (read from the environment at backend creation):
//   MOCK_SUBDEVICE_COUNT             number of sub-devices (default 4)
//   MOCK_SIGNAL_LOSS_AFTER_FRAMES    frames until hasNoInputSource is raised
//   MOCK_SIGNAL_LOSS_DURATION_FRAMES how long the outage lasts (default 50)
//   MOCK_FORMAT_CHANGE_AFTER_FRAMES  frames until a format change to
//                                    MOCK_FORMAT_CHANGE_MODE (default HD720p50)
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "config/videomodes.hpp"
#include "decklink/device.hpp"
#include "util/logging.hpp"
#include "util/taiclock.hpp"
#include "util/threading.hpp"
#include "util/v210.hpp"

namespace mxldl::dl
{
    namespace
    {
        struct MockOptions
        {
            int subDeviceCount = 4;
            std::int64_t signalLossAfterFrames = -1;
            std::int64_t signalLossDurationFrames = 50;
            std::int64_t formatChangeAfterFrames = -1;
            std::string formatChangeMode = "HD720p50";
        };

        MockOptions readOptions(config::EnvReader const& env)
        {
            MockOptions opt;
            auto readInt = [&env](char const* name, std::int64_t fallback) {
                auto const v = env(name);
                if (!v || v->empty())
                {
                    return fallback;
                }
                return static_cast<std::int64_t>(std::stoll(*v));
            };
            opt.subDeviceCount = static_cast<int>(readInt("MOCK_SUBDEVICE_COUNT", 4));
            opt.signalLossAfterFrames = readInt("MOCK_SIGNAL_LOSS_AFTER_FRAMES", -1);
            opt.signalLossDurationFrames = readInt("MOCK_SIGNAL_LOSS_DURATION_FRAMES", 50);
            opt.formatChangeAfterFrames = readInt("MOCK_FORMAT_CHANGE_AFTER_FRAMES", -1);
            if (auto const v = env("MOCK_FORMAT_CHANGE_MODE"); v && !v->empty())
            {
                opt.formatChangeMode = *v;
            }
            return opt;
        }

        /// Fills a v210 frame with 8 vertical color bars plus a moving
        /// counter band encoded in the luma of the last 32 rows.
        void renderBars(std::vector<std::uint8_t>& frame, std::uint32_t width, std::uint32_t height, std::size_t rowBytes, std::uint64_t counter)
        {
            struct YCbCr
            {
                std::uint16_t y, cb, cr;
            };
            // 75% SMPTE bars (10-bit BT.709 levels).
            static constexpr YCbCr kBars[8] = {
                {721, 512, 512}, // white 75%
                {646, 176, 543}, // yellow
                {525, 625, 176}, // cyan
                {450, 289, 208}, // green
                {335, 735, 816}, // magenta
                {260, 399, 848}, // red
                {139, 848, 481}, // blue
                {64, 512, 512}, // black
            };

            frame.assign(rowBytes * height, 0);
            std::uint32_t const counterBandStart = height > 32 ? height - 32 : 0;

            for (std::uint32_t y = 0; y < height; ++y)
            {
                auto* row = reinterpret_cast<std::uint32_t*>(frame.data() + static_cast<std::size_t>(y) * rowBytes);
                std::uint32_t word = 0;
                // Component sequence Cb Y Cr Y Cb Y Cr Y…, 3 components/word.
                std::uint32_t const totalComponents = width * 2;
                std::uint32_t componentIdx = 0;
                std::uint32_t wordIdx = 0;
                std::uint32_t accum = 0;
                std::uint32_t accumCount = 0;
                while (componentIdx < totalComponents)
                {
                    std::uint32_t const pixel = componentIdx / 2;
                    auto const& bar = kBars[(pixel * 8) / width];
                    std::uint16_t component;
                    if (componentIdx % 2 == 1)
                    {
                        // Luma; encode the counter as alternating stripes in
                        // the bottom band so tests can assert frame advance.
                        if (y >= counterBandStart)
                        {
                            bool const bit = ((pixel / 16) + counter) % 2 == 0;
                            component = bit ? 940 : 64;
                        }
                        else
                        {
                            component = bar.y;
                        }
                    }
                    else
                    {
                        component = (componentIdx % 4 == 0) ? bar.cb : bar.cr;
                    }
                    accum |= static_cast<std::uint32_t>(component & 0x3ff) << (10 * accumCount);
                    ++accumCount;
                    if (accumCount == 3)
                    {
                        row[wordIdx++] = accum;
                        accum = 0;
                        accumCount = 0;
                    }
                    ++componentIdx;
                }
                if (accumCount != 0)
                {
                    row[wordIdx] = accum;
                }
                (void)word;
            }
        }

        // ------------------------------------------------------------------
        class MockCaptureSession final : public ICaptureSession
        {
        public:
            explicit MockCaptureSession(MockOptions options)
                : _options(std::move(options))
            {}

            ~MockCaptureSession() override
            {
                disable();
            }

            std::optional<std::string> enable(Params const& params, CaptureCallbacks callbacks) override
            {
                _params = params;
                _callbacks = std::move(callbacks);
                return std::nullopt;
            }

            std::optional<std::string> start() override
            {
                bool expected = false;
                if (!_running.compare_exchange_strong(expected, true))
                {
                    return std::nullopt;
                }
                _thread = std::thread([this] {
                    generatorLoop();
                });
                return std::nullopt;
            }

            void stop() override
            {
                _running.store(false);
                if (_thread.joinable())
                {
                    _thread.join();
                }
            }

            void resetStreams() override
            {
                _resets.fetch_add(1);
            }

            std::optional<std::string> changeMode(config::VideoMode const& mode) override
            {
                std::lock_guard const lock{_modeMutex};
                _params.mode = mode;
                _modeChanged.store(true);
                return std::nullopt;
            }

            void disable() override
            {
                stop();
            }

            bool signalLocked() override
            {
                return !_inSignalLoss.load();
            }

            std::optional<std::uint64_t> hardwareClockNowNs() override
            {
                // The mock hardware clock is CLOCK_TAI shifted by a constant,
                // so hardware-timestamp calibration is exercised for real.
                return util::taiNowNs() - kHardwareClockOffsetNs;
            }

        private:
            static constexpr std::uint64_t kHardwareClockOffsetNs = 123'456'789'000ULL;

            void generatorLoop()
            {
                util::setThreadName("mock-capture");
                std::uint64_t frameCounter = 0;
                std::vector<std::uint8_t> videoFrame;
                std::vector<std::int32_t> audio32;
                std::vector<std::int16_t> audio16;
                double tonePhase = 0.0;

                while (_running.load())
                {
                    config::VideoMode mode;
                    {
                        std::lock_guard const lock{_modeMutex};
                        mode = _params.mode;
                    }
                    std::size_t const rowBytes =
                        _params.pixelFormat == config::PixelFormat::YUV8 ? mode.width * 2ULL : util::v210RowBytes(mode.width);

                    // Pace to the frame grid of CLOCK_TAI.
                    std::uint64_t const frameDurNs =
                        static_cast<std::uint64_t>(1'000'000'000ULL * mode.rateDenominator / static_cast<std::uint64_t>(mode.rateNumerator));
                    std::uint64_t const now = util::taiNowNs();
                    std::uint64_t const nextEdge = (now / frameDurNs + 1) * frameDurNs;
                    std::uint64_t const sleepNs = nextEdge - now;
                    std::this_thread::sleep_for(std::chrono::nanoseconds(sleepNs));
                    if (!_running.load())
                    {
                        break;
                    }

                    ++frameCounter;

                    // Fault injection: signal loss window.
                    bool signalLost = false;
                    if (_options.signalLossAfterFrames >= 0 && frameCounter > static_cast<std::uint64_t>(_options.signalLossAfterFrames) &&
                        frameCounter <= static_cast<std::uint64_t>(_options.signalLossAfterFrames + _options.signalLossDurationFrames))
                    {
                        signalLost = true;
                    }
                    _inSignalLoss.store(signalLost);

                    // Fault injection: format change (fires once).
                    if (_options.formatChangeAfterFrames >= 0 && frameCounter == static_cast<std::uint64_t>(_options.formatChangeAfterFrames) &&
                        _params.enableFormatDetection && _callbacks.onFormatChanged)
                    {
                        auto const newMode = config::lookupVideoMode(_options.formatChangeMode);
                        if (newMode && newMode->bmdDisplayMode != mode.bmdDisplayMode)
                        {
                            FormatChange fc;
                            fc.newMode = *newMode;
                            fc.is10Bit = true;
                            _callbacks.onFormatChanged(fc);
                            continue; // the channel re-enables via changeMode()
                        }
                    }

                    VideoFrameView view;
                    view.width = mode.width;
                    view.height = mode.height;
                    view.rowBytes = rowBytes;
                    view.hasNoInputSource = signalLost;
                    view.hasHardwareTimestamp = true;
                    view.hardwareTimestampNs = nextEdge - kHardwareClockOffsetNs;

                    if (!signalLost)
                    {
                        if (_params.pixelFormat == config::PixelFormat::YUV8)
                        {
                            videoFrame.assign(rowBytes * mode.height, 0x80);
                        }
                        else
                        {
                            renderBars(videoFrame, mode.width, mode.height, rowBytes, frameCounter);
                        }
                        view.bytes = videoFrame.data();

                        if (_params.collectAncPackets)
                        {
                            // One deterministic test packet per frame
                            // (DID/SDID 0x61/0x02 ≈ CEA-708 CDP style).
                            util::AncPacket p;
                            p.did = 0x61;
                            p.sdid = 0x02;
                            p.lineNumber = 9;
                            p.userData = {
                                static_cast<std::uint8_t>(frameCounter & 0xff),
                                static_cast<std::uint8_t>((frameCounter >> 8) & 0xff),
                                0xaa,
                                0x55,
                            };
                            view.ancPackets.push_back(std::move(p));
                        }
                    }

                    // Audio: 1 kHz tone at -18 dBFS, one frame's worth.
                    AudioPacketView audioView;
                    AudioPacketView const* audioPtr = nullptr;
                    if (_params.audioEnable && !signalLost)
                    {
                        std::size_t const samples = static_cast<std::size_t>(
                            48000ULL * mode.rateDenominator / static_cast<std::uint64_t>(mode.rateNumerator));
                        double const amplitude = 0.125; // ≈ -18 dBFS
                        std::size_t const chans = static_cast<std::size_t>(_params.audioChannelCount);
                        if (_params.audioSampleType == config::AudioSampleType::Int32)
                        {
                            audio32.resize(samples * chans);
                            for (std::size_t i = 0; i < samples; ++i)
                            {
                                auto const v = static_cast<std::int32_t>(amplitude * 2147483647.0 * std::sin(tonePhase));
                                tonePhase += 2.0 * M_PI * 1000.0 / 48000.0;
                                for (std::size_t c = 0; c < chans; ++c)
                                {
                                    audio32[i * chans + c] = v;
                                }
                            }
                            audioView.bytes = audio32.data();
                        }
                        else
                        {
                            audio16.resize(samples * chans);
                            for (std::size_t i = 0; i < samples; ++i)
                            {
                                auto const v = static_cast<std::int16_t>(amplitude * 32767.0 * std::sin(tonePhase));
                                tonePhase += 2.0 * M_PI * 1000.0 / 48000.0;
                                for (std::size_t c = 0; c < chans; ++c)
                                {
                                    audio16[i * chans + c] = v;
                                }
                            }
                            audioView.bytes = audio16.data();
                        }
                        audioView.sampleFrames = samples;
                        audioPtr = &audioView;
                    }

                    if (_callbacks.onFrame)
                    {
                        _callbacks.onFrame(view, audioPtr);
                    }
                }
            }

            MockOptions _options;
            Params _params;
            CaptureCallbacks _callbacks;
            std::atomic<bool> _running{false};
            std::atomic<bool> _inSignalLoss{false};
            std::atomic<bool> _modeChanged{false};
            std::atomic<int> _resets{0};
            std::mutex _modeMutex;
            std::thread _thread;
        };

        // ------------------------------------------------------------------
        class MockPlaybackSession final : public IPlaybackSession
        {
        public:
            ~MockPlaybackSession() override
            {
                disable();
            }

            std::optional<std::string> enable(Params const& params, PlaybackCallbacks callbacks) override
            {
                _params = params;
                _callbacks = std::move(callbacks);
                return std::nullopt;
            }

            std::optional<std::uint64_t> scheduleFrame(std::uint8_t const* /*bytes*/, std::size_t /*size*/, std::int64_t /*displayTime*/,
                std::int64_t /*displayDuration*/, std::int64_t /*timeScale*/) override
            {
                std::uint64_t const id = _nextFrameId.fetch_add(1);
                _bufferedFrames.fetch_add(1);
                return id;
            }

            std::optional<std::string> scheduleAudio(void const* /*pcm*/, std::uint32_t sampleFrames) override
            {
                _bufferedAudio.fetch_add(sampleFrames);
                return std::nullopt;
            }

            std::optional<std::string> beginAudioPreroll() override
            {
                if (_params.audioEnable && _callbacks.onRenderAudio)
                {
                    _callbacks.onRenderAudio(true);
                }
                return std::nullopt;
            }

            std::optional<std::string> endAudioPreroll() override
            {
                return std::nullopt;
            }

            std::optional<std::string> startPlayback(std::int64_t /*startTime*/, std::int64_t /*timeScale*/) override
            {
                bool expected = false;
                if (!_running.compare_exchange_strong(expected, true))
                {
                    return std::nullopt;
                }
                _thread = std::thread([this] {
                    playbackLoop();
                });
                return std::nullopt;
            }

            void stopPlayback() override
            {
                bool const wasRunning = _running.exchange(false);
                if (_thread.joinable())
                {
                    _thread.join();
                }
                if (wasRunning && _callbacks.onPlaybackStopped)
                {
                    _callbacks.onPlaybackStopped();
                }
            }

            void disable() override
            {
                stopPlayback();
            }

            std::uint32_t bufferedVideoFrames() override
            {
                return _bufferedFrames.load();
            }

            std::uint32_t bufferedAudioFrames() override
            {
                return _bufferedAudio.load();
            }

            bool referenceLocked() override
            {
                return true;
            }

        private:
            void playbackLoop()
            {
                util::setThreadName("mock-playback");
                std::uint64_t completedId = 0;
                std::uint64_t const frameDurNs = static_cast<std::uint64_t>(
                    1'000'000'000ULL * _params.mode.rateDenominator / static_cast<std::uint64_t>(_params.mode.rateNumerator));
                std::size_t const samplesPerFrame =
                    static_cast<std::size_t>(48000ULL * _params.mode.rateDenominator / static_cast<std::uint64_t>(_params.mode.rateNumerator));

                while (_running.load())
                {
                    std::uint64_t const now = util::taiNowNs();
                    std::uint64_t const nextEdge = (now / frameDurNs + 1) * frameDurNs;
                    std::this_thread::sleep_for(std::chrono::nanoseconds(nextEdge - now));
                    if (!_running.load())
                    {
                        break;
                    }

                    if (_bufferedFrames.load() > 0)
                    {
                        _bufferedFrames.fetch_sub(1);
                        ++completedId;
                        if (_callbacks.onFrameCompleted)
                        {
                            _callbacks.onFrameCompleted(completedId, CompletionResult::Completed);
                        }
                    }
                    auto const buffered = _bufferedAudio.load();
                    _bufferedAudio.store(buffered > samplesPerFrame ? static_cast<std::uint32_t>(buffered - samplesPerFrame) : 0U);
                    if (_params.audioEnable && _callbacks.onRenderAudio)
                    {
                        _callbacks.onRenderAudio(false);
                    }
                }
            }

            Params _params;
            PlaybackCallbacks _callbacks;
            std::atomic<bool> _running{false};
            std::atomic<std::uint64_t> _nextFrameId{1};
            std::atomic<std::uint32_t> _bufferedFrames{0};
            std::atomic<std::uint32_t> _bufferedAudio{0};
            std::thread _thread;
        };

        // ------------------------------------------------------------------
        class MockSubDevice final : public ISubDevice
        {
        public:
            MockSubDevice(MockOptions options, int index)
                : _options(std::move(options))
            {
                _info.persistentId = 0xa1b2c3d4;
                _info.deviceGroupId = 0xa1b2c3d4;
                _info.displayName = "Mock DeckLink (" + std::to_string(index + 1) + ")";
                _info.modelName = "Mock DeckLink";
                _info.subDeviceIndexOnCard = index;
                _info.supportsInputFormatDetection = true;
                _info.supportsCapture = true;
                _info.supportsPlayback = true;
                _info.hasProfileManager = false;
            }

            [[nodiscard]] SubDeviceInfo const& info() const override
            {
                return _info;
            }

            bool supportsMode(config::VideoMode const& /*mode*/, config::Direction /*direction*/) override
            {
                return true;
            }

            std::unique_ptr<ICaptureSession> openCapture() override
            {
                return std::make_unique<MockCaptureSession>(_options);
            }

            std::unique_ptr<IPlaybackSession> openPlayback() override
            {
                return std::make_unique<MockPlaybackSession>();
            }

        private:
            MockOptions _options;
            SubDeviceInfo _info;
        };

        class MockCard final : public ICard
        {
        public:
            explicit MockCard(MockOptions const& options)
                : _displayName("Mock DeckLink")
            {
                for (int i = 0; i < options.subDeviceCount; ++i)
                {
                    _subDevices.push_back(std::make_unique<MockSubDevice>(options, i));
                }
            }

            [[nodiscard]] std::string const& displayName() const override
            {
                return _displayName;
            }

            [[nodiscard]] std::uint32_t persistentId() const override
            {
                return 0xa1b2c3d4;
            }

            [[nodiscard]] std::size_t subDeviceCount() const override
            {
                return _subDevices.size();
            }

            ISubDevice& subDevice(std::size_t index) override
            {
                return *_subDevices.at(index);
            }

            std::optional<std::string> applyProfile(config::CardProfile profile) override
            {
                log::info("card_profile_applied", {{"card", _displayName}, {"profile", config::profileName(profile)}, {"mock", true}});
                return std::nullopt;
            }

            void setCallbacks(CardCallbacks callbacks) override
            {
                _callbacks = std::move(callbacks);
            }

        private:
            std::string _displayName;
            std::vector<std::unique_ptr<MockSubDevice>> _subDevices;
            CardCallbacks _callbacks;
        };

        class MockBackend final : public IBackend
        {
        public:
            explicit MockBackend(MockOptions options)
                : _options(std::move(options))
            {}

            std::variant<std::unique_ptr<ICard>, std::string> openCard(CardSelector const& selector) override
            {
                if (selector.persistentId && *selector.persistentId != 0xa1b2c3d4)
                {
                    char buf[32];
                    ::snprintf(buf, sizeof(buf), "0x%08x", *selector.persistentId);
                    return std::string("no DeckLink card matches MXL_DECKLINK_CARD_ID ") + buf + " (mock card is 0xa1b2c3d4)";
                }
                if (selector.displayName && std::string("Mock DeckLink").rfind(*selector.displayName, 0) != 0)
                {
                    return "no DeckLink card matches MXL_DECKLINK_CARD_NAME '" + *selector.displayName + "'";
                }
                if (selector.index && *selector.index != 0)
                {
                    return "MXL_DECKLINK_CARD_INDEX " + std::to_string(*selector.index) + " out of range (1 mock card)";
                }
                return std::unique_ptr<ICard>(std::make_unique<MockCard>(_options));
            }

        private:
            MockOptions _options;
        };
    }

    std::unique_ptr<IBackend> makeMockBackend(config::EnvReader const& env)
    {
        return std::make_unique<MockBackend>(readOptions(env));
    }
}
