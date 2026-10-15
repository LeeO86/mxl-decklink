// SPDX-License-Identifier: MIT
// Output channel: MXL flows → DeckLink scheduled playback (SPECIFICATION.md
// §2.4).
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "channel/reopen_schedule.hpp"
#include "channel/state.hpp"
#include "config/config.hpp"
#include "decklink/device.hpp"
#include "mxlbridge/audioreader.hpp"
#include "mxlbridge/domain.hpp"
#include "mxlbridge/flowsync.hpp"
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
        struct AudioFlowReader
        {
            config::AudioFlowConfig cfg;
            std::unique_ptr<mxlbridge::AudioReader> reader; // null when flow UUID is nil
            /// The writer re-created the flow: the reader is out of the sync
            /// group and not read until the supervisor re-opened it.
            bool flowInvalid = false;
            mxlFlowConfigInfo config{}; // at bring-up
        };

        void supervisorLoop();
        bool bringUp();
        void tearDownStreaming();
        void onFrameCompleted(std::uint64_t frameId, dl::CompletionResult result);
        void onRenderAudio(bool preroll);
        bool scheduleGrain(std::uint64_t grainIndex);
        void prerollResetRequested();

        // MXL_ERR_FLOW_INVALID handling (§2.4). The mark* and find* calls run
        // with _callbackMutex held (or before playback starts); they take the
        // reader out of the sync group and wake the supervisor.
        void markVideoFlowInvalid();
        void markAudioFlowInvalid(std::size_t slot);
        void findInvalidReaders();
        void requestReopen();
        /// Supervisor thread: release the invalid readers and open them again
        /// for the same flow ids.
        void reopenInvalidReaders();
        void waitForWork(std::chrono::milliseconds timeout);

        [[nodiscard]] mxlbridge::Domain& readerDomain();

        config::Config const& _globalCfg;
        config::ChannelConfig _cfg;
        dl::ISubDevice& _subDevice;
        mxlbridge::Domain& _domain;
        /// The channel's own MXL instance for its readers. MXL keeps one
        /// reader per flow id and instance and hands it out again while any
        /// holder keeps it, so a stale reader shared with another channel
        /// could not be re-opened.
        std::unique_ptr<mxlbridge::Domain> _readerDomain;
        std::string _cardIdLabel;

        Status _status;
        std::unique_ptr<dl::IPlaybackSession> _playback;
        std::unique_ptr<mxlbridge::VideoReader> _videoReader;
        /// See AudioFlowReader::flowInvalid; _videoReader may then be null.
        bool _videoFlowInvalid = false;
        mxlRational _videoRate{};
        mxlFlowConfigInfo _videoConfig{}; // at bring-up
        std::vector<AudioFlowReader> _audioFlows;
        /// Released before the readers: the group holds raw reader handles.
        std::unique_ptr<mxlbridge::FlowSyncGroup> _sync;

        config::VideoMode _mode{};
        std::atomic<bool> _playing{false};
        std::atomic<bool> _resetRequested{false};

        // Scheduling state (completion-driven, §2.4).
        std::mutex _scheduleMutex;
        /// Held by each DeckLink callback for its whole run; teardown takes it
        /// after stopping playback, before it frees the group and readers.
        std::mutex _callbackMutex;
        std::uint64_t _nextGrainIndex = 0;
        std::uint64_t _scheduledFrames = 0;
        std::vector<std::uint8_t> _lastFrame; // repeated on reader timeout
        // Int32-aligned PCM scratch (DeckLink sample type is usually 32-bit;
        // Int16 uses a parallel buffer to avoid misaligned casts).
        std::vector<std::int32_t> _audioScratchInt32;
        std::vector<std::int16_t> _audioScratchInt16;
        std::uint64_t _nextAudioEndIndex = 0;
        std::size_t _samplesPerFrame = 0;

        // Late/drop rate tracking for preroll reset (§2.4).
        std::uint64_t _recentLateOrDropped = 0;
        std::uint64_t _recentWindowStart = 0;

        std::thread _supervisor;
        std::atomic<bool> _running{false};
        std::atomic<bool> _streamingUp{false};

        // Wakes the supervisor when a reader saw its flow re-created.
        std::mutex _wakeMutex;
        std::condition_variable _wake;
        std::atomic<bool> _reopenRequested{false};
        ReopenSchedule _reopen; // supervisor thread only
        bool _reopenDegraded = false; // supervisor thread only

        ops::Counter* _framesTotal = nullptr;
        ops::Counter* _framesDropped = nullptr;
        ops::Counter* _framesLate = nullptr;
        ops::Counter* _reconnectTotal = nullptr;
        ops::Counter* _flowReopens = nullptr;
        ops::Gauge* _stateGauge = nullptr;
        ops::Gauge* _bufferedVideoGauge = nullptr;
        ops::Gauge* _bufferedAudioGauge = nullptr;
        ops::Gauge* _readerLagGauge = nullptr;

        void setState(State s);
    };
}
