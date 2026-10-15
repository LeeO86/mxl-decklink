// SPDX-License-Identifier: MIT
#include "output_channel.hpp"

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
        _framesLate = &metrics.counter("mxl_decklink_frames_late_total", "Frames displayed late", labels);
        _reconnectTotal = &metrics.counter("mxl_decklink_reconnect_total", "Channel reconnect attempts", labels);
        _flowReopens = &metrics.counter("mxl_decklink_flow_reopens_total", "MXL flows re-created by their writer; the reader was re-opened", labels);
        _stateGauge = &metrics.gauge("mxl_decklink_channel_state", "Channel state (0=init 1=healthy 2=degraded 3=failed)", labels);
        _bufferedVideoGauge = &metrics.gauge("mxl_decklink_buffered_video_frames", "Frames buffered in the device", labels);
        _bufferedAudioGauge = &metrics.gauge("mxl_decklink_buffered_audio_samples", "Audio sample frames buffered in the device", labels);
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
            _videoReader = std::make_unique<mxlbridge::VideoReader>(readerDomain(), _cfg.videoFlowId.toString());
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
                        slot.reader = std::make_unique<mxlbridge::AudioReader>(readerDomain(), afCfg.flowId.toString());
                        slot.config = slot.reader->configInfo();
                    }
                    // Nil UUID = unassigned: mapped DeckLink channels stay silent.
                    _audioFlows.push_back(std::move(slot));
                }
            }

            // The group gets its own instance of the readers' domain, so
            // other channels releasing readers never walk it (flowsync.hpp).
            _sync = std::make_unique<mxlbridge::FlowSyncGroup>(readerDomain().path());
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

            auto const rate = _videoRate;
            _samplesPerFrame = static_cast<std::size_t>(48000ULL * static_cast<std::uint64_t>(rate.denominator) /
                                                        static_cast<std::uint64_t>(rate.numerator));

            // §2.4 preroll: wait for data, then schedule
            // CHx_OUTPUT_PREROLL_GRAINS frames before starting playback.
            auto const currentIndex = ::mxlGetCurrentIndex(&rate);
            std::uint64_t const preroll = static_cast<std::uint64_t>(_cfg.outputPrerollGrains);
            {
                std::lock_guard const lock{_scheduleMutex};
                _nextGrainIndex = currentIndex > preroll ? currentIndex - preroll : 0;
                _scheduledFrames = 0;
                _recentLateOrDropped = 0;
                _recentWindowStart = util::taiNowNs();
                if (!_audioFlows.empty())
                {
                    _nextAudioEndIndex = _nextGrainIndex * _samplesPerFrame;
                }
            }

            for (std::uint64_t i = 0; i < preroll; ++i)
            {
                std::uint64_t idx = 0;
                {
                    std::lock_guard const lock{_scheduleMutex};
                    idx = _nextGrainIndex++;
                }
                if (!scheduleGrain(idx))
                {
                    log::warn("preroll_grain_unavailable",
                        {
                            {"channel_index", _cfg.index},
                            {"grain_index", idx},
                        });
                    tearDownStreaming();
                    return false;
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

    bool OutputChannel::scheduleGrain(std::uint64_t grainIndex)
    {
        mxlbridge::VideoReader::Grain grain;
        mxlStatus status = MXL_ERR_UNKNOWN;
        // While the supervisor re-opens a re-created video flow there is no
        // reader to read: the last frame is repeated below.
        if (_videoReader && !_videoFlowInvalid)
        {
            auto const timeoutNs = static_cast<std::uint64_t>(_cfg.readerTimeoutMs) * 1'000'000ULL;
            auto grainTimeoutNs = timeoutNs;
            if (_sync)
            {
                auto const timestamp = ::mxlIndexToTimestamp(&_videoRate, grainIndex);
                if (timestamp != std::numeric_limits<std::uint64_t>::max())
                {
                    // One wait covers the video grain and the audio sample at
                    // the same TAI time. A successful wait means the grain
                    // fetch below should not block.
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
                }
            }
            if (!_videoFlowInvalid)
            {
                status = _videoReader->getGrain(grainIndex, grainTimeoutNs, grain);
                if (status == MXL_ERR_FLOW_INVALID)
                {
                    markVideoFlowInvalid();
                }
            }
        }

        std::uint8_t const* bytes = nullptr;
        std::size_t size = 0;
        bool repeated = false;

        std::lock_guard const lock{_scheduleMutex};
        if (status == MXL_STATUS_OK && grain.payload != nullptr && (grain.info.flags & MXL_GRAIN_FLAG_INVALID) == 0)
        {
            bytes = grain.payload;
            size = grain.info.grainSize;
            _lastFrame.assign(bytes, bytes + size);
        }
        else if (!_lastFrame.empty())
        {
            // Reader timeout, invalid grain, or a re-created flow being
            // re-opened: repeat the previous frame so the output stays
            // continuous; counted as a drop (§2.4), but not towards the
            // late/drop preroll reset, which counts DeckLink completions only.
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
            _framesDropped->inc();
            _status.framesDropped.fetch_add(1);
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
            }
            else
            {
                _framesDropped->inc();
                _status.framesDropped.fetch_add(1);
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
        if (!scheduleGrain(idx))
        {
            _framesDropped->inc();
            _status.framesDropped.fetch_add(1);
        }
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
        auto const timeoutNs = static_cast<std::uint64_t>(_cfg.readerTimeoutMs) * 1'000'000ULL;
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
        for (std::size_t slot = 0; slot < _audioFlows.size(); ++slot)
        {
            auto& af = _audioFlows[slot];
            if (!af.reader || af.flowInvalid)
            {
                continue; // nil UUID, or a re-created flow being re-opened → silent
            }
            auto const status =
                af.reader->readSamples(_nextAudioEndIndex, _samplesPerFrame, timeoutNs, pcm, deckChannels, af.cfg.deckLinkChannels, _cfg.audioSampleType);
            if (status == MXL_STATUS_OK)
            {
                anyOk = true;
            }
            else if (status == MXL_ERR_OUT_OF_RANGE_TOO_EARLY)
            {
                tooEarly = true;
            }
            else if (status == MXL_ERR_FLOW_INVALID)
            {
                markAudioFlowInvalid(slot);
            }
        }

        if (anyOk)
        {
            _playback->scheduleAudio(pcm, static_cast<std::uint32_t>(_samplesPerFrame));
        }
        else if (tooEarly)
        {
            // Producer not there yet; skip this cycle and resync the index to
            // the video grain grid on the next successful video grain.
            _nextAudioEndIndex -= _samplesPerFrame;
        }
        else
        {
            // All assigned readers failed (or only nil slots): still schedule
            // silence so DeckLink audio clock stays filled.
            _playback->scheduleAudio(pcm, static_cast<std::uint32_t>(_samplesPerFrame));
        }
    }

    mxlbridge::Domain& OutputChannel::readerDomain()
    {
        // Always a private instance, also for this function's own domain:
        // re-opening a re-created flow needs the last reference to the stale
        // reader on the instance to go (see _readerDomain).
        auto const& path = _cfg.readerDomainPath.empty() ? _domain.path() : _cfg.readerDomainPath;
        if (!_readerDomain || _readerDomain->path() != path)
        {
            _readerDomain = std::make_unique<mxlbridge::Domain>(path);
        }
        return *_readerDomain;
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
                if (_sync)
                {
                    _sync->removeReader(af.reader->handle());
                }
                marked = true;
            }
        }
        if (marked)
        {
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
                setState(State::Degraded);
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
                newVideo = std::make_unique<mxlbridge::VideoReader>(readerDomain(), _cfg.videoFlowId.toString());
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
                auto reader = std::make_unique<mxlbridge::AudioReader>(readerDomain(), af.cfg.flowId.toString());
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
                setState(State::Degraded);
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
            setState(State::Healthy);
        }
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
