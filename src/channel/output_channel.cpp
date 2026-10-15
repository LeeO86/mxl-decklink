// SPDX-License-Identifier: MIT
#include "output_channel.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>

#include <mxl/time.h>

#include "util/logging.hpp"
#include "util/taiclock.hpp"
#include "util/threading.hpp"

namespace mxldl::channel
{
    namespace
    {
        constexpr std::uint64_t kBackoffMs[] = {500, 1000, 2000, 5000, 10000};

        std::uint64_t backoffForAttempt(std::size_t attempt)
        {
            constexpr std::size_t n = sizeof(kBackoffMs) / sizeof(kBackoffMs[0]);
            return kBackoffMs[attempt < n ? attempt : n - 1];
        }

        // §2.4: late/dropped rate above this threshold within the window
        // triggers a preroll reset.
        constexpr std::uint64_t kLateWindowNs = 10'000'000'000ULL;
        constexpr std::uint64_t kLateThreshold = 25;

        // §2.4: a read in the completion callback leaves the card at least
        // this much (or a quarter frame) before the frame is due.
        constexpr std::uint64_t kScheduleMarginNs = 4'000'000ULL;
        // A completion call that comes within this of the previous callback's
        // return was queued behind it.
        constexpr std::uint64_t kHeldUpNs = 1'000'000ULL;

        // §2.4: a source stall longer than this marks the channel degraded
        // (as a flow missing for that long does, reopen_schedule.hpp).
        constexpr std::uint64_t kStallDegradedNs = 1'000'000'000ULL;
        // A stall is logged once it lasted this long (shorter ones are only
        // counted in mxl_decklink_source_stalls_total).
        constexpr std::uint64_t kStallLogNs = 200'000'000ULL;
        // A source is stalled when a read finds nothing and its head has not
        // moved for this many frame periods, timed across calls: a live
        // writer that writes a frame's worth at a time moves it every period.
        constexpr std::uint64_t kStallPeriods = 2;
        // §2.4: a read more than this many grains (plus the read offset)
        // behind the source's newest grain skips ahead to it.
        constexpr std::uint64_t kCatchUpGrains = 2;
        // ... for this long in a row (a burst after an output-side delay
        // reads behind the head for a few frames only).
        constexpr std::uint64_t kCatchUpAfterNs = 1'000'000'000ULL;
        // §2.4: a source late for this long marks the channel degraded.
        constexpr std::uint64_t kLateDegradedNs = 1'000'000'000ULL;
        // A late run is logged once it has this many frames in a row.
        constexpr std::uint64_t kLateRunLog = 3;
        // Late repeats less than this apart belong to one run (a source
        // late every other frame is one run, not many short ones).
        constexpr std::uint64_t kLateRunGapNs = 1'000'000'000ULL;
        // Summed-up events (stall ends, catch-ups, read errors, lost frames)
        // are logged at most this often.
        constexpr std::uint64_t kEventLogIntervalNs = 1'000'000'000ULL;
        // _lastReadError for a grain its writer marked invalid.
        constexpr int kInvalidGrain = -1;

        std::string readErrorText(int status)
        {
            if (status == kInvalidGrain)
            {
                return "grain marked invalid by its writer";
            }
            if (status == MXL_ERR_OUT_OF_RANGE_TOO_LATE)
            {
                return "grain older than the flow's history (read position skipped ahead)";
            }
            if (status == MXL_ERR_OUT_OF_RANGE_TOO_EARLY)
            {
                return "grain older than the newest one but never written (gap)";
            }
            return "mxl status " + std::to_string(status);
        }

        std::uint64_t steadyNowNs()
        {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        bool sameRate(mxlRational const& a, mxlRational const& b)
        {
            return a.numerator == b.numerator && a.denominator == b.denominator;
        }

        // A re-created flow is re-opened in place only when the channel can
        // keep its playback session; otherwise the channel restarts.
        bool sameVideoFormat(mxlFlowConfigInfo const& a, mxlFlowConfigInfo const& b)
        {
            return a.common.format == b.common.format && sameRate(a.common.grainRate, b.common.grainRate) &&
                   a.discrete.sliceSizes[0] == b.discrete.sliceSizes[0];
        }

        bool sameAudioFormat(mxlFlowConfigInfo const& a, mxlFlowConfigInfo const& b)
        {
            return a.common.format == b.common.format && sameRate(a.common.grainRate, b.common.grainRate) &&
                   a.continuous.channelCount == b.continuous.channelCount;
        }
    }

    OutputChannel::OutputChannel(config::Config const& globalCfg, config::ChannelConfig const& chCfg, dl::ISubDevice& subDevice,
        mxlbridge::Domain& domain, ops::Registry& metrics, std::string cardIdLabel)
        : _globalCfg(globalCfg)
        , _cfg(chCfg)
        , _subDevice(subDevice)
        , _domain(domain)
        , _cardIdLabel(std::move(cardIdLabel))
    {
        _status.setActiveVideoFlowId(_cfg.videoFlowId.toString());

        ops::Labels const labels = {
            {"card_id", _cardIdLabel},
            {"channel_index", std::to_string(_cfg.index)},
            {"channel_label", _cfg.label},
            {"direction", "output"},
            {"subdevice_index", std::to_string(_cfg.subdeviceIndex)},
        };
        _framesTotal = &metrics.counter("mxl_decklink_frames_total", "Frames processed", labels);
        _framesDropped = &metrics.counter("mxl_decklink_frames_dropped_total", "Frames dropped", labels);
        auto const withLabel = [&labels](char const* name, char const* value) {
            auto out = labels;
            out.emplace_back(name, value);
            return out;
        };
        char const* const repeatedHelp = "Output frames that repeated the last frame because no new grain could be read";
        _repeatedSourceStall = &metrics.counter("mxl_decklink_frames_repeated_total", repeatedHelp, withLabel("reason", "source_stall"));
        _repeatedSourceLate = &metrics.counter("mxl_decklink_frames_repeated_total", repeatedHelp, withLabel("reason", "source_late"));
        _repeatedFlowReopen = &metrics.counter("mxl_decklink_frames_repeated_total", repeatedHelp, withLabel("reason", "flow_reopen"));
        _repeatedReadError = &metrics.counter("mxl_decklink_frames_repeated_total", repeatedHelp, withLabel("reason", "read_error"));
        char const* const stallsHelp = "Output source stalls: an MXL flow stopped getting new grains or samples";
        _stallsVideo = &metrics.counter("mxl_decklink_source_stalls_total", stallsHelp, withLabel("essence", "video"));
        _stallsAudio = &metrics.counter("mxl_decklink_source_stalls_total", stallsHelp, withLabel("essence", "audio"));
        _framesLate = &metrics.counter("mxl_decklink_frames_late_total", "Frames displayed late", labels);
        _reconnectTotal = &metrics.counter("mxl_decklink_reconnect_total", "Channel reconnect attempts", labels);
        _flowReopens = &metrics.counter("mxl_decklink_flow_reopens_total", "MXL flows re-created by their writer; the reader was re-opened", labels);
        _stateGauge = &metrics.gauge("mxl_decklink_channel_state", "Channel state (0=init 1=healthy 2=degraded 3=failed)", labels);
        _bufferedVideoGauge = &metrics.gauge("mxl_decklink_buffered_video_frames", "Frames buffered in the device", labels);
        _bufferedAudioGauge = &metrics.gauge("mxl_decklink_buffered_audio_samples", "Audio sample frames buffered in the device", labels);
        _readOffsetGauge = &metrics.gauge("mxl_decklink_read_offset_grains", "Output: grains read behind the newest grain (CHx_READ_OFFSET_GRAINS, capped)", labels);
        _readerLagGauge = &metrics.gauge("mxl_decklink_reader_lag_grains", "Reader lag behind the flow head in grains", labels);
    }

    OutputChannel::~OutputChannel()
    {
        stop();
    }

    void OutputChannel::setState(State s)
    {
        auto const prev = _status.state.exchange(s);
        _stateGauge->set(static_cast<double>(static_cast<int>(s)));
        if (prev != s)
        {
            log::info("channel_state_changed",
                {
                    {"channel_index", _cfg.index},
                    {"channel_label", _cfg.label},
                    {"direction", "output"},
                    {"from", stateName(prev)},
                    {"to", stateName(s)},
                });
        }
    }

    void OutputChannel::start()
    {
        _running.store(true);
        _supervisor = std::thread([this] {
            util::setThreadName("out-ch" + std::to_string(_cfg.index));
            if (_globalCfg.rtSched)
            {
                if (auto const err = util::setRealtimePriority(_globalCfg.realtimePriority))
                {
                    log::warn("rt_priority_failed", {{"channel_index", _cfg.index}, {"details", *err}});
                }
            }
            if (_globalCfg.cpuPinList)
            {
                if (auto const err = util::pinToCpus(*_globalCfg.cpuPinList))
                {
                    log::warn("cpu_pin_failed", {{"channel_index", _cfg.index}, {"details", *err}});
                }
            }
            supervisorLoop();
        });
    }

    void OutputChannel::stop()
    {
        {
            std::lock_guard const lock{_wakeMutex};
            _running.store(false);
        }
        _wake.notify_all();
        if (_supervisor.joinable())
        {
            _supervisor.join();
        }
        tearDownStreaming();
    }

    void OutputChannel::supervisorLoop()
    {
        std::size_t attempt = 0;
        while (_running.load())
        {
            if (!_cfg.videoMxlActive)
            {
                // IS-05 master_enable is false: hold the sub-device idle until
                // a later activation restarts this channel.
                setState(State::Init);
                while (_running.load())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
                return;
            }

            if (!_streamingUp.load())
            {
                if (attempt > 0)
                {
                    _reconnectTotal->inc();
                    _status.reconnects.fetch_add(1);
                }
                if (bringUp())
                {
                    attempt = 0;
                }
                else
                {
                    setState(State::Failed);
                    auto const waitMs = backoffForAttempt(attempt++);
                    log::warn("channel_bringup_failed",
                        {
                            {"channel_index", _cfg.index},
                            {"channel_label", _cfg.label},
                            {"retry_in_ms", waitMs},
                        });
                    for (std::uint64_t waited = 0; waited < waitMs && _running.load(); waited += 50)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    continue;
                }
            }

            if (_resetRequested.exchange(false))
            {
                log::warn("preroll_reset", {{"channel_index", _cfg.index}, {"channel_label", _cfg.label}});
                tearDownStreaming();
                continue;
            }

            reopenInvalidReaders();
            superviseStalls();
            waitForWork(std::chrono::milliseconds(100));
        }
    }

    void OutputChannel::waitForWork(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{_wakeMutex};
        _wake.wait_for(lock, timeout, [this] {
            return _reopenRequested.load() || !_running.load();
        });
    }

    bool OutputChannel::bringUp()
    {
        setState(State::Init);
        _videoFlowInvalid = false;
        _reopenRequested.store(false);
        _reopen.clear();
        _reopenDegraded = false;
        _videoStalled = false;
        {
            // A new session never repeats the previous one's picture.
            std::lock_guard const lock{_scheduleMutex};
            _lastFrame.clear();
        }
        _videoLastHead = 0;
        _videoHeadChangeNs = 0;
        _lateDegraded = false;
        _behindSinceNs = 0;
        _lateRunCount.store(0);
        _lateLastNs.store(0);
        _lateSinceNs.store(0);
        _stallSinceNs.store(0);
        _stallDegraded = false;
        _stallLogged = false;
        try
        {
            _mode = *_cfg.videoMode;
            _status.setActiveModeName(_mode.name);

            if (!_subDevice.supportsMode(_mode, config::Direction::Output))
            {
                log::error("video_mode_not_supported", {{"channel_index", _cfg.index}, {"video_mode", _mode.name}});
                return false;
            }

            // §2.4: reader handles first; producers may not be up yet — the
            // supervisor retries with backoff (§3.7).
            if (_cfg.videoFlowId.isNil())
            {
                log::warn("output_video_unassigned",
                    {
                        {"channel_index", _cfg.index},
                        {"details", "MXL_VIDEO_FLOW_ID is the nil UUID — assign a flow before the channel can start"},
                    });
                return false;
            }
            _videoReader = std::make_unique<mxlbridge::VideoReader>(readerDomain(_cfg.readerDomainPath), _cfg.videoFlowId.toString());
            _videoRate = _videoReader->grainRate();
            _videoConfig = _videoReader->configInfo();
            _audioFlows.clear();
            if (_cfg.audioEnable)
            {
                _audioFlows.reserve(_cfg.audioFlows.size());
                for (auto const& afCfg : _cfg.audioFlows)
                {
                    AudioFlowReader slot;
                    slot.cfg = afCfg;
                    if (!afCfg.flowId.isNil() && afCfg.mxlActive)
                    {
                        slot.reader = std::make_unique<mxlbridge::AudioReader>(readerDomain(afCfg.readerDomainPath), afCfg.flowId.toString());
                        slot.config = slot.reader->configInfo();
                    }
                    // Nil UUID = unassigned: mapped DeckLink channels stay silent.
                    _audioFlows.push_back(std::move(slot));
                }
            }

            // The group gets its own instance of the video's domain, so
            // other channels releasing readers never walk it (flowsync.hpp).
            // It takes readers of any instance, so audio from another domain
            // joins it too.
            _sync = std::make_unique<mxlbridge::FlowSyncGroup>(readerDomain(_cfg.readerDomainPath).path());
            _sync->addReader(_videoReader->handle());
            for (auto const& af : _audioFlows)
            {
                if (af.reader)
                {
                    _sync->addReader(af.reader->handle());
                }
            }

            _playback = _subDevice.openPlayback();
            if (!_playback)
            {
                log::error("open_playback_failed", {{"channel_index", _cfg.index}});
                tearDownStreaming();
                return false;
            }

            dl::IPlaybackSession::Params params;
            params.mode = _mode;
            params.pixelFormat = _cfg.pixelFormat;
            params.audioEnable = _cfg.audioEnable;
            params.audioChannelCount = _cfg.audioChannelCount;
            params.audioSampleType = _cfg.audioSampleType;

            dl::PlaybackCallbacks callbacks;
            callbacks.onFrameCompleted = [this](std::uint64_t id, dl::CompletionResult r) {
                onFrameCompleted(id, r);
            };
            callbacks.onRenderAudio = [this](bool preroll) {
                onRenderAudio(preroll);
            };
            callbacks.onPlaybackStopped = [this] {
                _playing.store(false);
            };

            if (auto const err = _playback->enable(params, std::move(callbacks)))
            {
                log::error("playback_enable_failed", {{"channel_index", _cfg.index}, {"details", *err}});
                tearDownStreaming();
                return false;
            }

            // §2.4: when a frame completes, the card still holds
            // CHx_OUTPUT_PREROLL_GRAINS - 1 frames. A read in that callback
            // waits at most that long, less a margin, or the card would show
            // the next frame late; a source later than that repeats the last
            // frame (reason source_late) instead.
            {
                auto const periodNs = 1'000'000'000ULL * static_cast<std::uint64_t>(_mode.rateDenominator) / static_cast<std::uint64_t>(_mode.rateNumerator);
                _periodNs = periodNs;
                auto const marginNs = std::max<std::uint64_t>(kScheduleMarginNs, periodNs / 4);
                auto const ahead = static_cast<std::uint64_t>(_cfg.outputPrerollGrains > 1 ? _cfg.outputPrerollGrains - 1 : 0) * periodNs;
                _frameWaitNs = std::min<std::uint64_t>(ahead > marginNs ? ahead - marginNs : 0, static_cast<std::uint64_t>(_cfg.readerTimeoutMs) * 1'000'000ULL);
            }

            auto const rate = _videoRate;
            _samplesPerFrame = static_cast<std::size_t>(48000ULL * static_cast<std::uint64_t>(rate.denominator) /
                                                        static_cast<std::uint64_t>(rate.numerator));

            // §2.4 preroll: wait for data, then schedule
            // CHx_OUTPUT_PREROLL_GRAINS frames before starting playback.
            // CHx_READ_OFFSET_GRAINS: that many grains further behind (§2.4).
            auto const currentIndex = ::mxlGetCurrentIndex(&rate);
            std::uint64_t const preroll = static_cast<std::uint64_t>(_cfg.outputPrerollGrains);
            capReadOffset(preroll);
            std::uint64_t const behind = preroll + static_cast<std::uint64_t>(_readOffset);
            std::uint64_t start = currentIndex > behind ? currentIndex - behind : 0;
            // A mirrored source arrives a grain or two late, and a mirror
            // created by this route has no history yet: start no later than
            // its newest grain allows (the preroll grains end at head -
            // offset).
            auto const sourceHead = _videoReader->headIndex();
            if (sourceHead != 0 && sourceHead + 1 < start + behind)
            {
                start = sourceHead + 1 > behind ? sourceHead + 1 - behind : 0;
            }
            {
                std::lock_guard const lock{_scheduleMutex};
                _nextGrainIndex = start;
                _scheduledFrames = 0;
                _lastCompletionNs = 0;
                _lastCallbackEndNs = 0;
                _readDeadlineNs = 0;
                _recentLateOrDropped = 0;
                _recentWindowStart = util::taiNowNs();
                if (!_audioFlows.empty())
                {
                    _nextAudioEndIndex = _nextGrainIndex * _samplesPerFrame;
                }
            }

            // Grains the source does not have (older than a new mirror, or a
            // gap) are passed over at once; one it has not written yet is
            // waited for. Only when nothing can be shown within the
            // deadline does the bring-up fail (and retry).
            auto const timeoutNs = static_cast<std::uint64_t>(_cfg.readerTimeoutMs) * 1'000'000ULL;
            auto const prerollDeadline = steadyNowNs() + timeoutNs + (preroll + 3) * _periodNs;
            bool shown = false;
            for (std::uint64_t scheduled = 0; scheduled < preroll;)
            {
                std::uint64_t idx = 0;
                {
                    std::lock_guard const lock{_scheduleMutex};
                    idx = _nextGrainIndex++;
                }
                auto const head = _videoReader ? _videoReader->headIndex() : 0;
                auto const now = steadyNowNs();
                auto const left = prerollDeadline > now ? prerollDeadline - now : 0;
                if (scheduleGrain(idx, idx < head ? 0 : std::min(timeoutNs, left)))
                {
                    if (!shown)
                    {
                        // Audio starts with the grain of the first frame
                        // shown. That is the one before the read position
                        // now, also when the read jumped ahead to the
                        // source's newest grain (resumeAt).
                        shown = true;
                        std::lock_guard const lock{_scheduleMutex};
                        if (!_audioFlows.empty())
                        {
                            _nextAudioEndIndex = (_nextGrainIndex - 1) * _samplesPerFrame;
                        }
                    }
                    ++scheduled;
                    continue;
                }
                if (steadyNowNs() >= prerollDeadline)
                {
                    log::warn("preroll_grain_unavailable",
                        {
                            {"channel_index", _cfg.index},
                            {"grain_index", idx},
                            {"head_index", head},
                            {"read_offset_grains", _readOffset},
                        });
                    tearDownStreaming();
                    return false;
                }
                if (idx >= head)
                {
                    // Not written yet: read it again (a stalled reader does
                    // not wait in the read: do not spin).
                    {
                        std::lock_guard const lock{_scheduleMutex};
                        _nextGrainIndex = idx;
                    }
                    if (steadyNowNs() - now < 1'000'000ULL)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                }
            }

            if (_cfg.audioEnable && !_audioFlows.empty())
            {
                if (auto const err = _playback->beginAudioPreroll())
                {
                    log::warn("audio_preroll_failed", {{"channel_index", _cfg.index}, {"details", *err}});
                }
                else
                {
                    _playback->endAudioPreroll();
                }
            }

            if (auto const err = _playback->startPlayback(0, _mode.rateNumerator * 1000))
            {
                log::error("playback_start_failed", {{"channel_index", _cfg.index}, {"details", *err}});
                tearDownStreaming();
                return false;
            }

            _playing.store(true);
            _streamingUp.store(true);
            setState(State::Healthy);
            log::info("output_channel_started",
                {
                    {"channel_index", _cfg.index},
                    {"channel_label", _cfg.label},
                    {"video_mode", _mode.name},
                    {"flow_id", _cfg.videoFlowId.toString()},
                    {"preroll_grains", _cfg.outputPrerollGrains},
                    {"sync_group", true},
                });
            return true;
        }
        catch (std::exception const& e)
        {
            log::warn("output_channel_bringup_exception", {{"channel_index", _cfg.index}, {"details", e.what()}});
            tearDownStreaming();
            return false;
        }
    }

    void OutputChannel::tearDownStreaming()
    {
        _streamingUp.store(false);
        _playing.store(false);
        if (_playback)
        {
            _playback->stopPlayback();
            _playback->disable();
        }
        // The SDK does not promise that no completion or audio callback is
        // still running after the stop: wait for one in flight, then take
        // what it uses. Later callbacks see _playing false or no readers.
        std::unique_ptr<mxlbridge::FlowSyncGroup> sync;
        std::unique_ptr<dl::IPlaybackSession> playback;
        std::unique_ptr<mxlbridge::VideoReader> videoReader;
        std::vector<AudioFlowReader> audioFlows;
        {
            std::lock_guard const lock{_callbackMutex};
            sync = std::move(_sync);
            playback = std::move(_playback);
            videoReader = std::move(_videoReader);
            _videoFlowInvalid = false;
            audioFlows.swap(_audioFlows);
        }
        // Freed outside the lock: releasing the SDK objects may wait for a
        // callback, and a callback waits for the lock. The group goes before
        // the readers it holds.
        sync.reset();
        playback.reset();
        videoReader.reset();
        audioFlows.clear();
    }

    bool OutputChannel::scheduleGrain(std::uint64_t grainIndex, std::uint64_t waitNs)
    {
        mxlbridge::VideoReader::Grain grain;
        mxlStatus status = MXL_ERR_UNKNOWN;
        // While the supervisor re-opens a re-created video flow there is no
        // reader to read: the last frame is repeated below.
        auto reason = RepeatReason::FlowReopen;
        // Change of the read position after this frame: -1 reads the same
        // grain again (source late), > 0 skips ahead (far behind the source).
        std::int64_t adjust = 0;
        if (_videoReader && !_videoFlowInvalid)
        {
            auto const timeoutNs = waitNs;
            // A stalled source is not waited for: each wait would hold this
            // DeckLink callback for CHx_READER_TIMEOUT_MS, the card would run
            // out of frames and report them late (§2.4 source stall).
            std::uint64_t grainTimeoutNs = _videoStalled ? 0 : timeoutNs;
            // Heads before the wait: when each source last wrote is timed
            // across calls (kStallPeriods).
            auto const beforeNs = steadyNowNs();
            noteHead(_videoReader->headIndex(), beforeNs, _videoLastHead, _videoHeadChangeNs);
            for (auto& af : _audioFlows)
            {
                if (af.reader && !af.flowInvalid && !af.stalled)
                {
                    noteHead(af.reader->headIndex(), beforeNs, af.lastHead, af.headChangeNs);
                }
            }
            if (_sync)
            {
                auto const timestamp = ::mxlIndexToTimestamp(&_videoRate, grainIndex);
                if (timestamp != std::numeric_limits<std::uint64_t>::max())
                {
                    // One wait covers the video grain and the audio sample at
                    // the same TAI time. Stalled readers are out of the
                    // group, so it does not wait for them (an empty group
                    // returns at once).
                    auto const waited = _sync->waitFor(timestamp, timeoutNs);
                    if (waited == MXL_STATUS_OK)
                    {
                        grainTimeoutNs = 0;
                    }
                    else if (waited == MXL_ERR_FLOW_INVALID)
                    {
                        // A reader of the group has a re-created flow; MXL
                        // says so only after the wait timed out. Find it and
                        // take it out of the group so later waits do not
                        // block on it.
                        findInvalidReaders();
                        grainTimeoutNs = 0;
                    }
                    else
                    {
                        // The wait used this frame's time: no second wait for
                        // the grain.
                        if (waited == MXL_ERR_OUT_OF_RANGE_TOO_EARLY || waited == MXL_ERR_TIMEOUT)
                        {
                            markStalledAudio();
                        }
                        grainTimeoutNs = 0;
                    }
                }
            }
            if (!_videoFlowInvalid)
            {
                status = _videoReader->getGrain(grainIndex, grainTimeoutNs, grain);
                if (status == MXL_ERR_FLOW_INVALID)
                {
                    markVideoFlowInvalid();
                }
                else
                {
                    auto const now = steadyNowNs();
                    auto const head = _videoReader->headIndex();
                    noteHead(head, now, _videoLastHead, _videoHeadChangeNs);
                    if ((status == MXL_ERR_OUT_OF_RANGE_TOO_EARLY || status == MXL_ERR_TIMEOUT) && grainIndex < head)
                    {
                        // The head moved past this grain after the read gave
                        // up: it may have been committed just now. Look once
                        // more before calling it a gap.
                        status = _videoReader->getGrain(grainIndex, 0, grain);
                    }
                    if (status == MXL_STATUS_OK)
                    {
                        reason = RepeatReason::ReadError; // when the grain is marked invalid
                        bool const wasStalled = _videoStalled;
                        setVideoStalled(false);
                        _lateSinceNs.store(0);
                        bool const invalid = grain.payload == nullptr || (grain.info.flags & MXL_GRAIN_FLAG_INVALID) != 0;
                        auto const offset = static_cast<std::uint64_t>(_readOffset);
                        if (invalid && (wasStalled || head > grainIndex + offset + 1))
                        {
                            // A gap the source marked invalid after it
                            // jumped ahead (it resumed at the current time):
                            // show its newest grain instead.
                            adjust = resumeAt(grainIndex, head, grain, status);
                        }
                        else
                        {
                            adjust = catchUp(grainIndex, head, false);
                        }
                    }
                    else if (status == MXL_ERR_OUT_OF_RANGE_TOO_EARLY || status == MXL_ERR_TIMEOUT)
                    {
                        if (_videoStalled)
                        {
                            reason = RepeatReason::SourceStall;
                            if (head != _videoStallHead)
                            {
                                // The source writes again: wait for the next
                                // grain as usual. A source that resumed ahead
                                // of the read position (a restarted origin
                                // writes the current time) left a gap of
                                // grains it never wrote: go to its newest
                                // grain and show it now, instead of reading
                                // the gap (invalid grains, repeats).
                                setVideoStalled(false);
                                adjust = resumeAt(grainIndex, head, grain, status);
                            }
                        }
                        else if (grainIndex < head)
                        {
                            // Older than the newest grain but not readable:
                            // the source never wrote it (a gap, or before a
                            // new mirror's first grain). Waiting cannot help.
                            reason = RepeatReason::ReadError;
                        }
                        else if (now - _videoHeadChangeNs >= kStallPeriods * _periodNs)
                        {
                            // No new grain for two frame periods: the source
                            // stopped.
                            reason = RepeatReason::SourceStall;
                            setVideoStalled(true);
                        }
                        else
                        {
                            // The source writes, but its grain comes later
                            // than the card can wait for: repeat this frame
                            // and read the same grain next time, so the read
                            // position slips behind the source's new latency
                            // instead of staying ahead of it for good.
                            reason = RepeatReason::SourceLate;
                            adjust = -1;
                            if (_playing.load())
                            {
                                noteLate(now);
                                std::uint64_t expected = 0;
                                _lateSinceNs.compare_exchange_strong(expected, now);
                            }
                        }
                    }
                    else
                    {
                        reason = RepeatReason::ReadError;
                        if (status == MXL_ERR_OUT_OF_RANGE_TOO_LATE)
                        {
                            // Fell out of the ring (the card ran slower than
                            // the source for long, or the source came back
                            // far ahead): skip to the source and show its
                            // grain now instead of repeating this frame.
                            adjust = resumeAt(grainIndex, head, grain, status);
                            if (status == MXL_STATUS_OK)
                            {
                                setVideoStalled(false);
                            }
                            else if (adjust == 0)
                            {
                                // The newest grain is not complete yet (a
                                // writer that commits slice by slice): move
                                // the read position anyway, or every later
                                // read would fall out of the history too.
                                adjust = catchUp(grainIndex, head, true);
                            }
                        }
                    }
                }
            }
        }

        std::uint8_t const* bytes = nullptr;
        std::size_t size = 0;
        bool repeated = false;

        std::lock_guard const lock{_scheduleMutex};
        auto const before = _nextGrainIndex;
        if (adjust < 0 && _nextGrainIndex > grainIndex)
        {
            _nextGrainIndex = grainIndex;
        }
        else if (adjust > 0)
        {
            _nextGrainIndex += static_cast<std::uint64_t>(adjust);
        }
        if (_nextGrainIndex != before && !_audioFlows.empty())
        {
            // The audio read position moves with the video's, so lip sync
            // holds: audio block = (video grain - (preroll - 1)) x samples
            // per frame. A slip plays one audio block again, a catch-up
            // skips as many blocks as grains.
            auto const delta = static_cast<std::int64_t>(_nextGrainIndex) - static_cast<std::int64_t>(before);
            _nextAudioEndIndex = static_cast<std::uint64_t>(static_cast<std::int64_t>(_nextAudioEndIndex) + delta * static_cast<std::int64_t>(_samplesPerFrame));
        }
        if (status == MXL_STATUS_OK && grain.payload != nullptr && (grain.info.flags & MXL_GRAIN_FLAG_INVALID) == 0)
        {
            bytes = grain.payload;
            size = grain.info.grainSize;
            _lastFrame.assign(bytes, bytes + size);
        }
        else if (!_lastFrame.empty())
        {
            // A stalled source, an invalid grain, or a re-created flow being
            // re-opened: repeat the previous frame so the output stays
            // continuous. Counted by reason in frames_repeated_total, not as
            // a drop, and never towards the late/drop preroll reset, which
            // counts DeckLink completions only (§2.4).
            bytes = _lastFrame.data();
            size = _lastFrame.size();
            repeated = true;
        }
        else
        {
            return false;
        }

        auto const displayTime = static_cast<std::int64_t>(_scheduledFrames) * _mode.rateDenominator * 1000;
        auto const frameId = _playback->scheduleFrame(bytes, size, displayTime, _mode.rateDenominator * 1000, _mode.rateNumerator * 1000);
        if (!frameId)
        {
            return false;
        }
        ++_scheduledFrames;
        _framesTotal->inc();
        _status.framesTotal.fetch_add(1);
        if (repeated)
        {
            switch (reason)
            {
                case RepeatReason::SourceStall: _repeatedSourceStall->inc(); break;
                case RepeatReason::SourceLate: _repeatedSourceLate->inc(); break;
                case RepeatReason::FlowReopen: _repeatedFlowReopen->inc(); break;
                case RepeatReason::ReadError:
                    _repeatedReadError->inc();
                    _readErrors.fetch_add(1);
                    _lastReadError.store(status == MXL_STATUS_OK ? kInvalidGrain : static_cast<int>(status));
                    break;
            }
            _status.framesRepeated.fetch_add(1);
        }
        else
        {
            _status.lastFrameTaiNs.store(util::taiNowNs());
            _status.grainsCommitted.fetch_add(1);
        }
        return true;
    }

    void OutputChannel::onFrameCompleted(std::uint64_t /*frameId*/, dl::CompletionResult result)
    {
        auto const deliveredNs = steadyNowNs(); // before the lock: when the card called
        std::lock_guard const callbackLock{_callbackMutex};
        if (!_playing.load())
        {
            return;
        }

        if (result == dl::CompletionResult::DisplayedLate || result == dl::CompletionResult::Dropped)
        {
            if (result == dl::CompletionResult::DisplayedLate)
            {
                _framesLate->inc();
                _cardLate.fetch_add(1);
            }
            else
            {
                _framesDropped->inc();
                _status.framesDropped.fetch_add(1);
                _cardDropped.fetch_add(1);
            }
            // Preroll reset when the late/drop rate exceeds the threshold.
            auto const now = util::taiNowNs();
            {
                std::lock_guard const lock{_scheduleMutex};
                if (now - _recentWindowStart > kLateWindowNs)
                {
                    _recentWindowStart = now;
                    _recentLateOrDropped = 0;
                }
                if (++_recentLateOrDropped >= kLateThreshold)
                {
                    _resetRequested.store(true);
                    return;
                }
            }
        }

        std::uint64_t idx = 0;
        {
            std::lock_guard const lock{_scheduleMutex};
            idx = _nextGrainIndex++;
        }
        // When did this frame complete? The call time, when the call came
        // while no completion callback ran. A call that came right as the
        // previous callback returned was held up by it (it waited for a late
        // or stalled source): then one frame after the previous completion.
        // The read waits at most until that time plus _frameWaitNs, so waits
        // in consecutive callbacks do not add up beyond what the card holds.
        auto const periodNs = 1'000'000'000ULL * static_cast<std::uint64_t>(_mode.rateDenominator) / static_cast<std::uint64_t>(_mode.rateNumerator);
        auto completedNs = deliveredNs;
        if (_lastCompletionNs != 0 && deliveredNs <= _lastCallbackEndNs + kHeldUpNs)
        {
            completedNs = std::min<std::uint64_t>(deliveredNs, _lastCompletionNs + periodNs);
        }
        _lastCompletionNs = completedNs;
        _readDeadlineNs = completedNs + _frameWaitNs;
        if (!scheduleGrain(idx, readBudgetNs()))
        {
            _framesDropped->inc();
            _status.framesDropped.fetch_add(1);
            _notScheduled.fetch_add(1);
        }
        _lastCallbackEndNs = steadyNowNs();
    }

    std::uint64_t OutputChannel::readBudgetNs() const
    {
        if (_readDeadlineNs == 0)
        {
            return _frameWaitNs; // no frame completed yet
        }
        auto const now = steadyNowNs();
        return _readDeadlineNs > now ? std::min(_readDeadlineNs - now, _frameWaitNs) : 0;
    }

    void OutputChannel::onRenderAudio(bool /*preroll*/)
    {
        std::lock_guard const callbackLock{_callbackMutex};
        if (_audioFlows.empty() || !_playback)
        {
            return;
        }
        std::lock_guard const lock{_scheduleMutex};
        _nextAudioEndIndex += _samplesPerFrame;
        auto const deckChannels = static_cast<std::size_t>(_cfg.audioChannelCount);
        // Same deadline as the last video read: this callback holds
        // _callbackMutex, so its wait delays the next video completion.
        auto const timeoutNs = readBudgetNs();
        auto const sampleCount = _samplesPerFrame * deckChannels;

        void* pcm = nullptr;
        if (_cfg.audioSampleType == config::AudioSampleType::Int32)
        {
            _audioScratchInt32.assign(sampleCount, 0);
            pcm = _audioScratchInt32.data();
        }
        else
        {
            _audioScratchInt16.assign(sampleCount, 0);
            pcm = _audioScratchInt16.data();
        }

        bool anyOk = false;
        bool tooEarly = false;
        bool tooLate = false;
        for (std::size_t slot = 0; slot < _audioFlows.size(); ++slot)
        {
            auto& af = _audioFlows[slot];
            if (!af.reader || af.flowInvalid)
            {
                continue; // nil UUID, or a re-created flow being re-opened → silent
            }
            // A stalled flow is read without waiting, so it cannot hold the
            // DeckLink callbacks (§2.4 source stall); its channels stay silent.
            noteHead(af.reader->headIndex(), steadyNowNs(), af.lastHead, af.headChangeNs);
            auto const status = af.reader->readSamples(_nextAudioEndIndex, _samplesPerFrame, af.stalled ? 0 : timeoutNs, pcm, deckChannels,
                af.cfg.deckLinkChannels, _cfg.audioSampleType);
            if (status == MXL_ERR_FLOW_INVALID)
            {
                markAudioFlowInvalid(slot);
                continue;
            }
            auto const now = steadyNowNs();
            auto const head = af.reader->headIndex();
            noteHead(head, now, af.lastHead, af.headChangeNs);
            bool const missing = status == MXL_ERR_OUT_OF_RANGE_TOO_EARLY || status == MXL_ERR_TIMEOUT;
            if (af.stalled)
            {
                // Samples written before the source stopped are still played
                // (the audio read position trails the video's). The stall
                // ends when the source writes again; a block it has not
                // written yet is then read again next time, not lost.
                anyOk = anyOk || status == MXL_STATUS_OK;
                if (head != af.stallHead)
                {
                    setAudioStalled(slot, false);
                    tooEarly = tooEarly || missing;
                }
            }
            else if (status == MXL_STATUS_OK)
            {
                anyOk = true;
            }
            else if (missing)
            {
                if (now - af.headChangeNs >= kStallPeriods * _periodNs)
                {
                    // No new sample for two frame periods: the source stopped.
                    setAudioStalled(slot, true);
                }
                else
                {
                    tooEarly = true; // the source writes; this block is ahead of it
                }
            }
            else if (status == MXL_ERR_OUT_OF_RANGE_TOO_LATE)
            {
                tooLate = true;
            }
        }

        if (anyOk || !tooEarly)
        {
            // Silent where nothing could be read (unassigned, re-opening or
            // stalled flows): the card's audio clock stays filled and the
            // sample index stays on the video's TAI grid, so audio is in sync
            // again when a stalled source resumes.
            _playback->scheduleAudio(pcm, static_cast<std::uint32_t>(_samplesPerFrame));
        }
        else
        {
            // A live source not there yet: skip this cycle and read the same
            // block next time.
            _nextAudioEndIndex -= _samplesPerFrame;
        }
        if (tooLate && !anyOk)
        {
            // Fell out of the ring: back to the video's read position (the
            // block played with the frame shown now, as at the start).
            auto const behind = static_cast<std::uint64_t>(_cfg.outputPrerollGrains > 1 ? _cfg.outputPrerollGrains - 1 : 0);
            _nextAudioEndIndex = (_nextGrainIndex > behind ? _nextGrainIndex - behind : 0) * _samplesPerFrame;
        }
    }

    mxlbridge::Domain& OutputChannel::readerDomain(std::string const& legDomainPath)
    {
        // Always a private instance, also for this function's own domain:
        // re-opening a re-created flow needs the last reference to the stale
        // reader on the instance to go (see _readerDomains). One instance per
        // domain: a reader opens a flow of its instance's domain only.
        auto const& path = legDomainPath.empty() ? _domain.path() : legDomainPath;
        for (auto const& d : _readerDomains)
        {
            if (d->path() == path)
            {
                return *d;
            }
        }
        // Another function's domain is opened, never created: a restored
        // route waits until that function made it (§7.7).
        auto const missing = path == _domain.path() ? mxlbridge::Domain::Missing::Create : mxlbridge::Domain::Missing::Fail;
        _readerDomains.push_back(std::make_unique<mxlbridge::Domain>(path, missing));
        return *_readerDomains.back();
    }

    void OutputChannel::requestReopen()
    {
        {
            std::lock_guard const lock{_wakeMutex};
            _reopenRequested.store(true);
        }
        _wake.notify_one();
    }

    void OutputChannel::markVideoFlowInvalid()
    {
        if (_videoFlowInvalid || !_videoReader)
        {
            return;
        }
        _videoFlowInvalid = true;
        if (_sync)
        {
            _sync->removeReader(_videoReader->handle());
        }
        // The re-open takes over from a stall (repeats count as flow_reopen).
        _videoStalled = false;
        updateStallClock();
        requestReopen();
    }

    void OutputChannel::markAudioFlowInvalid(std::size_t slot)
    {
        // Slots that read the same flow share one MXL reader: all of them go,
        // or the re-open would get the stale reader back.
        auto const flowId = _audioFlows[slot].cfg.flowId;
        bool marked = false;
        for (auto& af : _audioFlows)
        {
            if (af.reader && !af.flowInvalid && af.cfg.flowId == flowId)
            {
                af.flowInvalid = true;
                af.stalled = false;
                if (_sync)
                {
                    _sync->removeReader(af.reader->handle());
                }
                marked = true;
            }
        }
        if (marked)
        {
            updateStallClock();
            requestReopen();
        }
    }

    void OutputChannel::findInvalidReaders()
    {
        if (_videoReader && !_videoFlowInvalid && _videoReader->flowInvalid())
        {
            markVideoFlowInvalid();
        }
        for (std::size_t slot = 0; slot < _audioFlows.size(); ++slot)
        {
            auto const& af = _audioFlows[slot];
            if (af.reader && !af.flowInvalid && af.reader->flowInvalid())
            {
                markAudioFlowInvalid(slot);
            }
        }
    }

    void OutputChannel::reopenInvalidReaders()
    {
        auto const now = steadyNowNs();
        if (_reopenRequested.exchange(false))
        {
            _reopen.flowInvalid(now);
        }
        if (!_reopen.pending())
        {
            return;
        }
        if (!_streamingUp.load())
        {
            // A restart opens every reader anyway.
            _reopen.clear();
            return;
        }
        if (!_reopen.due(now))
        {
            if (!_reopenDegraded && _reopen.degraded(now))
            {
                _reopenDegraded = true;
                updateRunningState();
            }
            return;
        }

        // Take the invalid readers out; the callbacks already took them out
        // of the sync group and no longer read them.
        std::unique_ptr<mxlbridge::VideoReader> oldVideo;
        std::vector<std::unique_ptr<mxlbridge::AudioReader>> oldAudio;
        bool wantVideo = false;
        std::vector<std::size_t> wantAudio;
        std::vector<std::size_t> newAudioEvents;
        {
            std::lock_guard const lock{_callbackMutex};
            if (_videoFlowInvalid)
            {
                wantVideo = true;
                oldVideo = std::move(_videoReader);
            }
            for (std::size_t slot = 0; slot < _audioFlows.size(); ++slot)
            {
                auto& af = _audioFlows[slot];
                if (af.flowInvalid)
                {
                    wantAudio.push_back(slot);
                    if (af.reader)
                    {
                        newAudioEvents.push_back(slot);
                        oldAudio.push_back(std::move(af.reader));
                    }
                }
            }
        }

        // A reader still held here is a new event: log and count it once per
        // flow (slots sharing a flow share the event).
        auto const announce = [this](char const* essence, util::Uuid const& flowId) {
            _flowReopens->inc();
            log::info("mxl_flow_recreated",
                {
                    {"channel_index", _cfg.index},
                    {"channel_label", _cfg.label},
                    {"essence", essence},
                    {"flow_id", flowId.toString()},
                    {"details", "MXL_ERR_FLOW_INVALID: the writer re-created the flow; re-opening the reader"},
                });
        };
        if (oldVideo)
        {
            announce("video", _cfg.videoFlowId);
        }
        std::vector<util::Uuid> announced;
        for (auto const slot : newAudioEvents)
        {
            auto const& id = _audioFlows[slot].cfg.flowId;
            bool seen = false;
            for (auto const& a : announced)
            {
                seen = seen || a == id;
            }
            if (!seen)
            {
                announced.push_back(id);
                announce("audio", id);
            }
        }

        // Release first: the instance hands out its cached reader of a flow id
        // as long as it holds one.
        oldVideo.reset();
        oldAudio.clear();

        std::string error;
        bool formatChanged = false;
        std::unique_ptr<mxlbridge::VideoReader> newVideo;
        std::vector<std::pair<std::size_t, std::unique_ptr<mxlbridge::AudioReader>>> newAudio;
        if (wantVideo)
        {
            try
            {
                newVideo = std::make_unique<mxlbridge::VideoReader>(readerDomain(_cfg.readerDomainPath), _cfg.videoFlowId.toString());
                formatChanged = formatChanged || !sameVideoFormat(newVideo->configInfo(), _videoConfig);
            }
            catch (std::exception const& e)
            {
                error = e.what();
            }
        }
        for (auto const slot : wantAudio)
        {
            auto const& af = _audioFlows[slot];
            try
            {
                auto reader = std::make_unique<mxlbridge::AudioReader>(readerDomain(af.cfg.readerDomainPath), af.cfg.flowId.toString());
                formatChanged = formatChanged || !sameAudioFormat(reader->configInfo(), af.config);
                newAudio.emplace_back(slot, std::move(reader));
            }
            catch (std::exception const& e)
            {
                error = e.what();
            }
        }

        if (formatChanged)
        {
            // Another format under the same id: restart the channel, which
            // opens every reader and the playback session again.
            log::warn("mxl_flow_recreated_format_changed", {{"channel_index", _cfg.index}, {"channel_label", _cfg.label}});
            _reopen.clear();
            _resetRequested.store(true);
            return;
        }

        try
        {
            std::lock_guard const lock{_callbackMutex};
            if (newVideo)
            {
                _videoReader = std::move(newVideo);
                _videoFlowInvalid = false;
                if (_sync)
                {
                    _sync->addReader(_videoReader->handle());
                }
            }
            for (auto& [slot, reader] : newAudio)
            {
                auto& af = _audioFlows[slot];
                af.reader = std::move(reader);
                af.flowInvalid = false;
                af.stalled = false;
                if (_sync)
                {
                    _sync->addReader(af.reader->handle());
                }
            }
        }
        catch (std::exception const& e)
        {
            log::warn("mxl_flow_reopen_failed", {{"channel_index", _cfg.index}, {"channel_label", _cfg.label}, {"details", e.what()}});
            _reopen.clear();
            _resetRequested.store(true);
            return;
        }

        if (!error.empty())
        {
            // The flow is gone or not re-created yet: try again shortly.
            if (_reopen.failures() == 0)
            {
                log::warn("mxl_flow_reopen_failed",
                    {
                        {"channel_index", _cfg.index},
                        {"channel_label", _cfg.label},
                        {"retry_in_ms", ReopenSchedule::retryDelayNs(0) / 1'000'000ULL},
                        {"details", error},
                    });
            }
            _reopen.openFailed(now);
            if (!_reopenDegraded && _reopen.degraded(now))
            {
                _reopenDegraded = true;
                updateRunningState();
            }
            return;
        }

        log::info("mxl_flow_reopened",
            {
                {"channel_index", _cfg.index},
                {"channel_label", _cfg.label},
                {"attempts", _reopen.failures() + 1},
                {"after_ms", _reopen.pendingForNs(steadyNowNs()) / 1'000'000ULL},
            });
        _reopen.clear();
        if (_reopenDegraded)
        {
            _reopenDegraded = false;
            updateRunningState();
        }
    }

    void OutputChannel::updateRunningState()
    {
        if (!_streamingUp.load())
        {
            return;
        }
        setState(_reopenDegraded || _stallDegraded || _lateDegraded ? State::Degraded : State::Healthy);
    }

    void OutputChannel::setVideoStalled(bool stalled)
    {
        if (_videoStalled == stalled || !_videoReader)
        {
            return;
        }
        _videoStalled = stalled;
        _videoStallHead = stalled ? _videoReader->headIndex() : 0;
        if (_sync)
        {
            try
            {
                if (stalled)
                {
                    _sync->removeReader(_videoReader->handle());
                }
                else
                {
                    _sync->addReader(_videoReader->handle());
                }
            }
            catch (std::exception const& e)
            {
                log::warn("sync_group_add_failed", {{"channel_index", _cfg.index}, {"essence", "video"}, {"details", e.what()}});
            }
        }
        if (stalled)
        {
            _stallsVideo->inc();
        }
        updateStallClock();
    }

    void OutputChannel::setAudioStalled(std::size_t slot, bool stalled)
    {
        auto const& reader = _audioFlows[slot].reader;
        if (!reader || _audioFlows[slot].stalled == stalled)
        {
            return;
        }
        // Slots that read the same flow share one MXL reader (and one group
        // entry): they stall and resume together.
        auto const handle = reader->handle();
        auto const head = stalled ? reader->headIndex() : 0;
        for (auto& af : _audioFlows)
        {
            if (af.reader && !af.flowInvalid && af.reader->handle() == handle)
            {
                af.stalled = stalled;
                af.stallHead = head;
            }
        }
        if (_sync)
        {
            try
            {
                if (stalled)
                {
                    _sync->removeReader(handle);
                }
                else
                {
                    _sync->addReader(handle);
                }
            }
            catch (std::exception const& e)
            {
                log::warn("sync_group_add_failed", {{"channel_index", _cfg.index}, {"essence", "audio"}, {"details", e.what()}});
            }
        }
        if (stalled)
        {
            _stallsAudio->inc();
        }
        updateStallClock();
    }

    void OutputChannel::markStalledAudio()
    {
        // Video decides on its own grain read. An audio flow that wrote
        // nothing for two frame periods stopped, even when it still has the
        // sample at this frame's time (the audio read trails the video):
        // out of the group, or every later wait would block on it, and read
        // without waiting, or its own read would hold the callbacks. A live
        // flow taken for stalled loses nothing: a read without waiting still
        // gets the samples that are there, and the next write ends the stall.
        auto const now = steadyNowNs();
        for (std::size_t slot = 0; slot < _audioFlows.size(); ++slot)
        {
            auto& af = _audioFlows[slot];
            if (af.reader && !af.flowInvalid && !af.stalled)
            {
                noteHead(af.reader->headIndex(), now, af.lastHead, af.headChangeNs);
                if (now - af.headChangeNs >= kStallPeriods * _periodNs)
                {
                    setAudioStalled(slot, true);
                }
            }
        }
    }

    void OutputChannel::updateStallClock()
    {
        bool any = _videoStalled;
        for (auto const& af : _audioFlows)
        {
            any = any || (af.stalled && af.reader && !af.flowInvalid);
        }
        auto const since = _stallSinceNs.load();
        if (any && since == 0)
        {
            _stallRepeatsAtStart.store(static_cast<std::uint64_t>(_repeatedSourceStall->value()));
            _stallSinceNs.store(steadyNowNs());
        }
        else if (!any && since != 0)
        {
            _lastStallNs.store(steadyNowNs() - since);
            _endedStallRepeats.fetch_add(static_cast<std::uint64_t>(_repeatedSourceStall->value()) - _stallRepeatsAtStart.load());
            _stallsEnded.fetch_add(1);
            _stallSinceNs.store(0);
        }
    }

    void OutputChannel::superviseStalls()
    {
        auto const since = _stallSinceNs.load();
        auto const now = steadyNowNs();
        auto const due = [now](std::uint64_t last) {
            return last == 0 || now - last >= kEventLogIntervalNs;
        };

        // Every stall is logged: one that lasts 200 ms when it starts, all
        // of them when they end (at most one line a second, summed up).
        if (since != 0 && !_stallLogged && now - since >= kStallLogNs)
        {
            log::warn("source_stall",
                {
                    {"channel_index", _cfg.index},
                    {"channel_label", _cfg.label},
                    {"details", "an MXL flow gets no new grains or samples: video repeats the last frame, stalled audio is silent"},
                });
            _stallLogged = true;
        }
        auto const ended = _stallsEnded.load();
        if (ended != _stallsEndedLogged && (_stallLogged || due(_stallEndLogNs)))
        {
            auto const repeats = _endedStallRepeats.load();
            log::info("source_stall_ended",
                {
                    {"channel_index", _cfg.index},
                    {"channel_label", _cfg.label},
                    {"stalls", ended - _stallsEndedLogged},
                    {"duration_ms", _lastStallNs.load() / 1'000'000ULL}, // the last one
                    {"frames_repeated", repeats - _endedStallRepeatsLogged},
                });
            _stallsEndedLogged = ended;
            _endedStallRepeatsLogged = repeats;
            _stallEndLogNs = now;
            _stallLogged = false;
        }
        bool const degraded = since != 0 && now - since >= kStallDegradedNs;
        if (degraded != _stallDegraded)
        {
            _stallDegraded = degraded;
            updateRunningState();
        }

        // A run of late grains (late repeats less than 1 s apart): logged
        // when it has kLateRunLog repeats and when it ends, with the frames
        // repeated (each one slipped the read position by a grain). One that
        // lasts 1 s marks the channel degraded.
        auto const runId = _lateRunId.load();
        auto const runCount = _lateRunCount.load();
        auto const lastLate = _lateLastNs.load();
        if (runId != _lateRunStartLogged && runCount >= kLateRunLog)
        {
            log::warn("source_late",
                {
                    {"channel_index", _cfg.index},
                    {"channel_label", _cfg.label},
                    {"late_frames", runCount},
                    {"action", "slip: each late frame is repeated and the same grain read again next frame"},
                });
            _lateRunStartLogged = runId;
        }
        bool const runOver = lastLate != 0 && now > lastLate && now - lastLate >= kLateRunGapNs;
        if (_lateRunStartLogged != _lateRunEndLogged && (runId != _lateRunStartLogged || runOver))
        {
            auto const total = runId == _lateRunStartLogged ? runCount : _prevLateRunCount.load();
            log::info("source_late_ended",
                {
                    {"channel_index", _cfg.index},
                    {"channel_label", _cfg.label},
                    {"frames_repeated", total},
                    {"read_position_slipped_grains", total},
                });
            _lateRunEndLogged = _lateRunStartLogged;
        }
        // Degraded only while no grain came in time for 1 s: a jittery source
        // late now and then stays healthy (and in /readyz).
        auto const lateSince = _lateSinceNs.load();
        bool const late = lateSince != 0 && now > lateSince && now - lateSince >= kLateDegradedNs;
        if (late != _lateDegraded)
        {
            _lateDegraded = late;
            updateRunningState();
        }

        // Catch-ups, read errors and frames the card lost: summed up, at most
        // one line a second each.
        auto const catchUps = _catchUps.load();
        if (catchUps != _catchUpsLogged && due(_catchUpLogNs))
        {
            auto const grains = _catchUpGrains.load();
            log::info("read_position_caught_up",
                {
                    {"channel_index", _cfg.index},
                    {"channel_label", _cfg.label},
                    {"catch_ups", catchUps - _catchUpsLogged},
                    {"skipped_grains", grains - _catchUpGrainsLogged},
                    {"reason", _lastCatchUpOutOfRing.load() ? "source_jumped_ahead" : "behind_the_source"},
                });
            _catchUpsLogged = catchUps;
            _catchUpGrainsLogged = grains;
            _catchUpLogNs = now;
        }
        auto const readErrors = _readErrors.load();
        if (readErrors != _readErrorsLogged && due(_readErrorLogNs))
        {
            auto const last = _lastReadError.load();
            log::warn("grain_read_error",
                {
                    {"channel_index", _cfg.index},
                    {"channel_label", _cfg.label},
                    {"frames_repeated", readErrors - _readErrorsLogged},
                    {"last", readErrorText(last)},
                });
            _readErrorsLogged = readErrors;
            _readErrorLogNs = now;
        }
        auto const late2 = _cardLate.load();
        auto const dropped = _cardDropped.load();
        auto const notScheduled = _notScheduled.load();
        if ((late2 != _cardLateLogged || dropped != _cardDroppedLogged || notScheduled != _notScheduledLogged) && due(_cardLogNs))
        {
            log::warn("output_frames_lost",
                {
                    {"channel_index", _cfg.index},
                    {"channel_label", _cfg.label},
                    {"displayed_late", late2 - _cardLateLogged},
                    {"dropped_by_card", dropped - _cardDroppedLogged},
                    {"not_scheduled", notScheduled - _notScheduledLogged},
                    {"details", "output-side: the card showed frames late or dropped them, or a frame could not be scheduled"},
                });
            _cardLateLogged = late2;
            _cardDroppedLogged = dropped;
            _notScheduledLogged = notScheduled;
            _cardLogNs = now;
        }
    }

    std::int64_t OutputChannel::resumeAt(std::uint64_t grainIndex, std::uint64_t head, mxlbridge::VideoReader::Grain& grain, mxlStatus& status)
    {
        auto const offset = static_cast<std::uint64_t>(_readOffset);
        if (head < offset || head - offset <= grainIndex)
        {
            return 0;
        }
        auto const target = head - offset;
        mxlbridge::VideoReader::Grain newest;
        auto const read = _videoReader->getGrain(target, 0, newest);
        if (read != MXL_STATUS_OK || newest.payload == nullptr || (newest.info.flags & MXL_GRAIN_FLAG_INVALID) != 0)
        {
            return 0; // keep the read position; the next frame reads on
        }
        grain = newest;
        _lateSinceNs.store(0);
        status = read;
        auto const skip = target - grainIndex;
        _catchUpGrains.fetch_add(skip);
        _lastCatchUpOutOfRing.store(true);
        _catchUps.fetch_add(1);
        return static_cast<std::int64_t>(skip);
    }

    void OutputChannel::noteLate(std::uint64_t nowNs)
    {
        auto const last = _lateLastNs.load();
        if (last == 0 || nowNs - last > kLateRunGapNs)
        {
            _prevLateRunCount.store(_lateRunCount.load());
            _lateRunCount.store(0);
            _lateRunStartNs.store(nowNs);
            _lateRunId.fetch_add(1);
        }
        _lateRunCount.fetch_add(1);
        _lateLastNs.store(nowNs);
    }

    void OutputChannel::capReadOffset(std::uint64_t preroll)
    {
        // The read position at start is preroll + offset grains behind the
        // newest grain. MXL keeps grainCount - 2 grains readable for video
        // (PosixDiscreteFlowReader minIndex) and bufferLength / 2 samples for
        // audio; one grain / one frame of samples stays spare.
        auto const requested = static_cast<std::int64_t>(_cfg.readOffsetGrains);
        std::int64_t cap = requested;
        std::uint32_t grainCount = 0;
        std::uint32_t bufferLength = 0;
        if (_videoReader)
        {
            grainCount = _videoReader->grainCount();
            cap = std::min<std::int64_t>(cap, static_cast<std::int64_t>(grainCount) - 3 - static_cast<std::int64_t>(preroll));
        }
        for (auto const& af : _audioFlows)
        {
            if (af.reader && _samplesPerFrame > 0)
            {
                auto const length = af.reader->bufferLength();
                bufferLength = bufferLength == 0 ? length : std::min(bufferLength, length);
                auto const frames = static_cast<std::int64_t>(length / 2 / _samplesPerFrame);
                cap = std::min<std::int64_t>(cap, frames - 1 - static_cast<std::int64_t>(preroll));
            }
        }
        _readOffset = static_cast<int>(std::max<std::int64_t>(cap, 0));
        _status.readOffsetGrains.store(_readOffset);
        _readOffsetGauge->set(static_cast<double>(_readOffset));
        log::info("read_offset_applied",
            {
                {"channel_index", _cfg.index},
                {"channel_label", _cfg.label},
                {"requested", _cfg.readOffsetGrains},
                {"effective", _readOffset},
            });
        if (_readOffset != _cfg.readOffsetGrains)
        {
            log::warn("read_offset_capped",
                {
                    {"channel_index", _cfg.index},
                    {"channel_label", _cfg.label},
                    {"requested", _cfg.readOffsetGrains},
                    {"effective", _readOffset},
                    {"preroll_grains", _cfg.outputPrerollGrains},
                    {"video_grain_count", grainCount},
                    {"audio_buffer_length", bufferLength},
                    {"details", "CHx_READ_OFFSET_GRAINS + CHx_OUTPUT_PREROLL_GRAINS must stay inside the flows' history"},
                });
        }
    }

    void OutputChannel::noteHead(std::uint64_t head, std::uint64_t nowNs, std::uint64_t& lastHead, std::uint64_t& changedNs)
    {
        if (head != lastHead || changedNs == 0)
        {
            lastHead = head;
            changedNs = nowNs;
        }
    }

    std::int64_t OutputChannel::catchUp(std::uint64_t grainIndex, std::uint64_t head, bool now)
    {
        // Normally the newest grain is at most one ahead of the read position
        // plus the read offset. Much further behind for a sustained time (the
        // card ran slower than the source, or the source's latency shrank
        // after slips): skip ahead so the output does not lag ever more or
        // fall out of the ring. A short burst of completions after an
        // output-side delay also reads behind the head for a few frames: it
        // must not skip real grains, hence the time. Out of the ring
        // (`now`) it skips at once. The preroll reads further back on
        // purpose.
        if (!_playing.load())
        {
            return 0;
        }
        auto const offset = static_cast<std::uint64_t>(_readOffset);
        if (head <= grainIndex + offset + kCatchUpGrains)
        {
            _behindSinceNs = 0;
            return 0;
        }
        auto const t = steadyNowNs();
        if (_behindSinceNs == 0)
        {
            _behindSinceNs = t;
        }
        if (!now && t - _behindSinceNs < kCatchUpAfterNs)
        {
            return 0;
        }
        _behindSinceNs = 0;
        auto const skip = head - grainIndex - offset - 1;
        _catchUpGrains.fetch_add(skip);
        _lastCatchUpOutOfRing.store(now);
        _catchUps.fetch_add(1);
        return static_cast<std::int64_t>(skip);
    }

    void OutputChannel::housekeeping()
    {
        if (!_streamingUp.load() || !_playback)
        {
            return;
        }
        _bufferedVideoGauge->set(static_cast<double>(_playback->bufferedVideoFrames()));
        auto const audioBuffered = _playback->bufferedAudioFrames();
        _bufferedAudioGauge->set(static_cast<double>(audioBuffered));
        _status.bufferedVideoFrames.store(_playback->bufferedVideoFrames());
        _status.bufferedAudioFrames.store(audioBuffered);
        _status.signalLocked.store(_playback->referenceLocked());

        // The supervisor swaps the video reader when its flow is re-created.
        std::uint64_t head = 0;
        {
            std::lock_guard const lock{_callbackMutex};
            if (!_videoReader || _videoFlowInvalid)
            {
                return;
            }
            head = _videoReader->headIndex();
        }
        std::uint64_t next = 0;
        {
            std::lock_guard const lock{_scheduleMutex};
            next = _nextGrainIndex;
        }
        _readerLagGauge->set(head > next ? static_cast<double>(head - next) : 0.0);
    }
}
