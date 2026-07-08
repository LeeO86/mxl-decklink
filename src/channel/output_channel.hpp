// SPDX-License-Identifier: MIT
// Output channel: MXL flows → DeckLink scheduled playback (SPECIFICATION.md
// §2.4).
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "channel/state.hpp"
#include "config/config.hpp"
#include "decklink/device.hpp"
#include "mxlbridge/audioreader.hpp"
#include "mxlbridge/domain.hpp"
#include "mxlbridge/videoreader.hpp"
#include "ops/metrics.hpp"

namespace mxldl::channel
{
    class OutputChannel
    {
    public:
        OutputChannel(config::Config const& globalCfg, config::ChannelConfig const& chCfg, dl::ISubDevice& subDevice, mxlbridge::Domain& domain,
            ops::Registry& metrics, std::string cardIdLabel);
        ~OutputChannel();

        OutputChannel(OutputChannel const&) = delete;
        OutputChannel& operator=(OutputChannel const&) = delete;

        void start();
        void stop();

        [[nodiscard]] Status const& status() const
        {
            return _status;
        }

        [[nodiscard]] config::ChannelConfig const& channelConfig() const
        {
            return _cfg;
        }

        void housekeeping();

    private:
        void supervisorLoop();
        bool bringUp();
        void tearDownStreaming();
        void onFrameCompleted(std::uint64_t frameId, dl::CompletionResult result);
        void onRenderAudio(bool preroll);
        bool scheduleGrain(std::uint64_t grainIndex);
        void prerollResetRequested();

        config::Config const& _globalCfg;
        config::ChannelConfig _cfg;
        dl::ISubDevice& _subDevice;
        mxlbridge::Domain& _domain;
        std::string _cardIdLabel;

        Status _status;
        std::unique_ptr<dl::IPlaybackSession> _playback;
        std::unique_ptr<mxlbridge::VideoReader> _videoReader;
        std::unique_ptr<mxlbridge::AudioReader> _audioReader;

        config::VideoMode _mode{};
        std::atomic<bool> _playing{false};
        std::atomic<bool> _resetRequested{false};

        // Scheduling state (completion-driven, §2.4).
        std::mutex _scheduleMutex;
        std::uint64_t _nextGrainIndex = 0;
        std::uint64_t _scheduledFrames = 0;
        std::vector<std::uint8_t> _lastFrame; // repeated on reader timeout
        std::vector<std::uint8_t> _audioScratch;
        std::uint64_t _nextAudioEndIndex = 0;
        std::size_t _samplesPerFrame = 0;

        // Late/drop rate tracking for preroll reset (§2.4).
        std::uint64_t _recentLateOrDropped = 0;
        std::uint64_t _recentWindowStart = 0;

        std::thread _supervisor;
        std::atomic<bool> _running{false};
        std::atomic<bool> _streamingUp{false};

        ops::Counter* _framesTotal = nullptr;
        ops::Counter* _framesDropped = nullptr;
        ops::Counter* _framesLate = nullptr;
        ops::Counter* _reconnectTotal = nullptr;
        ops::Gauge* _stateGauge = nullptr;
        ops::Gauge* _bufferedVideoGauge = nullptr;
        ops::Gauge* _bufferedAudioGauge = nullptr;
        ops::Gauge* _readerLagGauge = nullptr;

        void setState(State s);
    };
}
