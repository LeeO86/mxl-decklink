// SPDX-License-Identifier: MIT
#include "output_channel.hpp"

#include <chrono>

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
        _stateGauge = &metrics.gauge("mxl_decklink_channel_state", "Channel state (0=init 1=healthy 2=degraded 3=failed)", labels);
        _bufferedVideoGauge = &metrics.gauge("mxl_buffered_video_frames", "Frames buffered in the device", labels);
        _bufferedAudioGauge = &metrics.gauge("mxl_buffered_audio_samples", "Audio sample frames buffered in the device", labels);
        _readerLagGauge = &metrics.gauge("mxl_ringbuffer_reader_lag_grains", "Reader lag behind the flow head in grains", labels);
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
        _running.store(false);
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

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    bool OutputChannel::bringUp()
    {
        setState(State::Init);
        try
        {
            _mode = *_cfg.videoMode;

            if (!_subDevice.supportsMode(_mode, config::Direction::Output))
            {
                log::error("video_mode_not_supported", {{"channel_index", _cfg.index}, {"video_mode", _mode.name}});
                return false;
            }

            // §2.4: reader handles first; producers may not be up yet — the
            // supervisor retries with backoff (§3.7).
            _videoReader = std::make_unique<mxlbridge::VideoReader>(_domain, _cfg.videoFlowId.toString());
            if (_cfg.audioEnable && _cfg.audioFlowId)
            {
                _audioReader = std::make_unique<mxlbridge::AudioReader>(_domain, _cfg.audioFlowId->toString());
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

            auto const rate = _videoReader->grainRate();
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
                if (_audioReader)
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

            if (_cfg.audioEnable && _audioReader)
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
            _playback.reset();
        }
        _videoReader.reset();
        _audioReader.reset();
    }

    bool OutputChannel::scheduleGrain(std::uint64_t grainIndex)
    {
        mxlbridge::VideoReader::Grain grain;
        auto const timeoutNs = static_cast<std::uint64_t>(_cfg.readerTimeoutMs) * 1'000'000ULL;
        auto const status = _videoReader->getGrain(grainIndex, timeoutNs, grain);

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
            // Reader timeout or invalid grain: repeat the previous frame so
            // the output stays continuous; counted as a drop (§2.4).
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
        if (!_audioReader || !_playback)
        {
            return;
        }
        std::lock_guard const lock{_scheduleMutex};
        _nextAudioEndIndex += _samplesPerFrame;
        _audioScratch.resize(_samplesPerFrame * static_cast<std::size_t>(_cfg.audioChannelCount) *
                             (_cfg.audioSampleType == config::AudioSampleType::Int32 ? 4 : 2));
        auto const timeoutNs = static_cast<std::uint64_t>(_cfg.readerTimeoutMs) * 1'000'000ULL;
        auto const status = _audioReader->readSamples(_nextAudioEndIndex, _samplesPerFrame, timeoutNs, _audioScratch.data(),
            static_cast<std::size_t>(_cfg.audioChannelCount), _cfg.audioSampleType);
        if (status == MXL_STATUS_OK)
        {
            _playback->scheduleAudio(_audioScratch.data(), static_cast<std::uint32_t>(_samplesPerFrame));
        }
        else if (status == MXL_ERR_OUT_OF_RANGE_TOO_EARLY)
        {
            // Producer not there yet; skip this cycle and resync the index to
            // the video grain grid on the next successful video grain.
            _nextAudioEndIndex -= _samplesPerFrame;
        }
    }

    void OutputChannel::housekeeping()
    {
        if (!_streamingUp.load() || !_playback || !_videoReader)
        {
            return;
        }
        _bufferedVideoGauge->set(static_cast<double>(_playback->bufferedVideoFrames()));
        _bufferedAudioGauge->set(static_cast<double>(_playback->bufferedAudioFrames()));
        _status.signalLocked.store(_playback->referenceLocked());

        auto const head = _videoReader->headIndex();
        std::uint64_t next = 0;
        {
            std::lock_guard const lock{_scheduleMutex};
            next = _nextGrainIndex;
        }
        _readerLagGauge->set(head > next ? static_cast<double>(head - next) : 0.0);
    }
}
