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
            /// The source stopped writing (§2.4 source stall): the reader is
            /// out of the sync group and read without waiting; its DeckLink
            /// channels play silence until samples come again.
            bool stalled = false;
            std::uint64_t stallHead = 0; // head index when the stall began
            std::uint64_t lastHead = 0; // head index last seen
            std::uint64_t headChangeNs = 0; // steady ns when lastHead last changed
            mxlFlowConfigInfo config{}; // at bring-up
        };

        /// Why a frame was repeated instead of read (§2.4); a label of
        /// mxl_decklink_frames_repeated_total.
        enum class RepeatReason
        {
            SourceStall, // the source stopped writing; the flow is still there
            SourceLate, // the source writes, but its grain came too late for the card
            FlowReopen, // the flow was re-created; its reader is being re-opened
            ReadError, // an invalid grain or another read error
        };

        void supervisorLoop();
        bool bringUp();
        void tearDownStreaming();
        void onFrameCompleted(std::uint64_t frameId, dl::CompletionResult result);
        void onRenderAudio(bool preroll);
        /// Reads grain `grainIndex` (waiting at most `waitNs`) and schedules
        /// it, or repeats the last frame.
        bool scheduleGrain(std::uint64_t grainIndex, std::uint64_t waitNs);
        /// How long a read may still wait (_callbackMutex held).
        [[nodiscard]] std::uint64_t readBudgetNs() const;
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

        // Source stalls (§2.4). Called with _callbackMutex held.
        void setVideoStalled(bool stalled);
        void setAudioStalled(std::size_t slot, bool stalled);
        /// After a sync-group wait timed out: the audio readers whose source
        /// wrote nothing during the wait are stalled.
        void markStalledAudio();
        void updateStallClock();
        /// A late repeat: counts it into the current run of late grains.
        void noteLate(std::uint64_t nowNs);
        /// Records when a reader's head index last changed.
        static void noteHead(std::uint64_t head, std::uint64_t nowNs, std::uint64_t& lastHead, std::uint64_t& changedNs);
        /// Grains to skip ahead after reading `grainIndex` with the source's
        /// newest grain at `head` (0 = none; §2.4 read position).
        /// `now`: out of the ring, skip at once (else only after it lasted).
        [[nodiscard]] std::int64_t catchUp(std::uint64_t grainIndex, std::uint64_t head, bool now);
        /// Bring-up: CHx_READ_OFFSET_GRAINS within the flows' history.
        void capReadOffset(std::uint64_t preroll);
        /// The source jumped ahead of the read position (it resumed after a
        /// stall, or the read fell out of its history): read its newest grain
        /// (less the read offset) into `grain`/`status` and return the grains
        /// skipped; 0 when there is nothing newer to read.
        [[nodiscard]] std::int64_t resumeAt(std::uint64_t grainIndex, std::uint64_t head, mxlbridge::VideoReader::Grain& grain, mxlStatus& status);
        /// Supervisor thread: logs stall start/end and keeps the channel
        /// `degraded` while a stall lasts longer than 1 s.
        void superviseStalls();
        /// Supervisor thread: healthy or degraded from the re-open and stall
        /// conditions.
        void updateRunningState();

        /// The channel's MXL instance of one domain directory (empty = this
        /// function's domain), created on first use and kept for the life of
        /// the channel.
        [[nodiscard]] mxlbridge::Domain& readerDomain(std::string const& legDomainPath);

        config::Config const& _globalCfg;
        config::ChannelConfig _cfg;
        dl::ISubDevice& _subDevice;
        mxlbridge::Domain& _domain;
        /// The channel's own MXL instances for its readers, one per domain
        /// directory its receivers read (video and audio may come from
        /// different domains). MXL keeps one reader per flow id and instance
        /// and hands it out again while any holder keeps it, so a stale reader
        /// shared with another channel could not be re-opened. Never shrinks
        /// while the channel lives: readers refer to their instance.
        std::vector<std::unique_ptr<mxlbridge::Domain>> _readerDomains;
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
        std::uint64_t _frameWaitNs = 0; // longest read wait in a completion callback
        // Read deadline of the completion callbacks (§2.4), _callbackMutex.
        std::uint64_t _lastCompletionNs = 0; // steady ns, estimated
        std::uint64_t _lastCallbackEndNs = 0; // when the previous completion callback returned
        std::uint64_t _readDeadlineNs = 0; // 0 = no frame completed yet
        std::vector<std::uint8_t> _lastFrame; // repeated when no grain can be read
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

        // Source stalls (§2.4): flags under _callbackMutex, clock atomics for
        // the supervisor.
        bool _videoStalled = false;
        std::uint64_t _videoStallHead = 0; // head index when the stall began
        std::uint64_t _videoLastHead = 0; // head index last seen
        std::uint64_t _videoHeadChangeNs = 0; // steady ns when it last changed
        std::uint64_t _periodNs = 20'000'000ULL; // frame period, set at bring-up
        int _readOffset = 0; // CHx_READ_OFFSET_GRAINS, capped at bring-up
        std::uint64_t _behindSinceNs = 0; // catch-up condition held since (steady ns), _callbackMutex
        bool _lateDegraded = false; // supervisor thread only
        std::atomic<std::uint64_t> _stallRepeatsAtStart{0};
        std::atomic<std::uint64_t> _stallsEnded{0};
        std::atomic<std::uint64_t> _endedStallRepeats{0}; // source_stall repeats of all finished stalls
        std::uint64_t _stallsEndedLogged = 0; // supervisor thread only
        std::uint64_t _endedStallRepeatsLogged = 0; // supervisor thread only
        std::uint64_t _stallEndLogNs = 0; // supervisor thread only

        // Late runs, catch-ups, read errors and lost frames, for the
        // supervisor's log lines (§7.4). Counters written by the callbacks.
        std::atomic<std::uint64_t> _lateRunId{0};
        std::atomic<std::uint64_t> _lateRunCount{0}; // late repeats in the current run
        std::atomic<std::uint64_t> _prevLateRunCount{0};
        std::atomic<std::uint64_t> _lateRunStartNs{0};
        std::atomic<std::uint64_t> _lateSinceNs{0}; // first late repeat since the last grain read in time; 0 = none
        std::atomic<std::uint64_t> _lateLastNs{0}; // steady ns of the last late repeat
        std::atomic<std::uint64_t> _catchUps{0};
        std::atomic<std::uint64_t> _catchUpGrains{0};
        std::atomic<bool> _lastCatchUpOutOfRing{false};
        std::atomic<std::uint64_t> _readErrors{0};
        std::atomic<int> _lastReadError{0};
        std::atomic<std::uint64_t> _cardLate{0};
        std::atomic<std::uint64_t> _cardDropped{0};
        std::atomic<std::uint64_t> _notScheduled{0};
        std::uint64_t _lateRunStartLogged = 0; // supervisor thread only, as the rest
        std::uint64_t _lateRunEndLogged = 0;
        std::uint64_t _catchUpsLogged = 0;
        std::uint64_t _catchUpGrainsLogged = 0;
        std::uint64_t _catchUpLogNs = 0;
        std::uint64_t _readErrorsLogged = 0;
        std::uint64_t _readErrorLogNs = 0;
        std::uint64_t _cardLateLogged = 0;
        std::uint64_t _cardDroppedLogged = 0;
        std::uint64_t _notScheduledLogged = 0;
        std::uint64_t _cardLogNs = 0;
        std::atomic<std::uint64_t> _stallSinceNs{0}; // steady ns; 0 = no reader stalled
        std::atomic<std::uint64_t> _lastStallNs{0}; // duration of the last finished stall
        bool _stallLogged = false; // supervisor thread only
        bool _stallDegraded = false; // supervisor thread only

        ops::Counter* _framesTotal = nullptr;
        ops::Counter* _framesDropped = nullptr;
        ops::Counter* _framesLate = nullptr;
        ops::Counter* _reconnectTotal = nullptr;
        ops::Counter* _flowReopens = nullptr;
        ops::Counter* _repeatedSourceStall = nullptr;
        ops::Counter* _repeatedSourceLate = nullptr;
        ops::Counter* _repeatedFlowReopen = nullptr;
        ops::Counter* _repeatedReadError = nullptr;
        ops::Counter* _stallsVideo = nullptr;
        ops::Counter* _stallsAudio = nullptr;
        ops::Gauge* _stateGauge = nullptr;
        ops::Gauge* _bufferedVideoGauge = nullptr;
        ops::Gauge* _bufferedAudioGauge = nullptr;
        ops::Gauge* _readerLagGauge = nullptr;
        ops::Gauge* _readOffsetGauge = nullptr;

        void setState(State s);
    };
}
