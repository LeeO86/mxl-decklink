// SPDX-License-Identifier: MIT
#include "input_channel.hpp"

#include <chrono>

#include <mxl/time.h>

#include "util/logging.hpp"
#include "util/threading.hpp"
#include "util/v210.hpp"

namespace mxldl::channel
{
    namespace
    {
        // §3.7: 500 ms → 1 s → 2 s → 5 s → 10 s, then constant 10 s.
        constexpr std::uint64_t kBackoffMs[] = {500, 1000, 2000, 5000, 10000};

        std::uint64_t backoffForAttempt(std::size_t attempt)
        {
            constexpr std::size_t n = sizeof(kBackoffMs) / sizeof(kBackoffMs[0]);
            return kBackoffMs[attempt < n ? attempt : n - 1];
        }

        // §3.8: grace period so readers can consume the last valid grain.
        constexpr auto kFormatChangeGrace = std::chrono::seconds(5);

        config::VideoMode autoStartMode()
        {
            // Auto-detect channels start with a common HD mode; the card
            // reports the real signal via VideoInputFormatChanged.
            return *config::lookupVideoMode("HD1080p50");
        }
    }

    InputChannel::InputChannel(config::Config const& globalCfg, config::ChannelConfig const& chCfg, dl::ISubDevice& subDevice,
        mxlbridge::Domain& domain, ops::Registry& metrics, std::string cardIdLabel)
        : _globalCfg(globalCfg)
        , _cfg(chCfg)
        , _subDevice(subDevice)
        , _domain(domain)
        , _cardIdLabel(std::move(cardIdLabel))
    {
        _useHardwareTimestamps.store(globalCfg.timestampSource == config::TimestampSourceCfg::Hardware);
        _status.setActiveVideoFlowId(_cfg.videoFlowId.toString());

        ops::Labels const labels = {
            {"card_id", _cardIdLabel},
            {"channel_index", std::to_string(_cfg.index)},
            {"channel_label", _cfg.label},
            {"direction", "input"},
            {"subdevice_index", std::to_string(_cfg.subdeviceIndex)},
        };
        _framesTotal = &metrics.counter("mxl_decklink_frames_total", "Frames processed", labels);
        _framesDropped = &metrics.counter("mxl_decklink_frames_dropped_total", "Frames dropped", labels);
        _signalLostTotal = &metrics.counter("mxl_decklink_signal_lost_total", "Signal loss events", labels);
        _formatChangesTotal = &metrics.counter("mxl_decklink_format_changes_total", "Input format changes", labels);
        _reconnectTotal = &metrics.counter("mxl_decklink_reconnect_total", "Channel reconnect attempts", labels);
        _grainsCommitted = &metrics.counter("mxl_grains_committed_total", "MXL grains committed", labels);
        _grainsWrittenBytes = &metrics.counter("mxl_grains_written_bytes_total", "Bytes written into MXL grains", labels);
        _stateGauge = &metrics.gauge("mxl_decklink_channel_state", "Channel state (0=init 1=healthy 2=degraded 3=failed)", labels);
        _signalLockGauge = &metrics.gauge("mxl_decklink_signal_lock", "Input signal lock (0/1)", labels);
        _headIndexGauge = &metrics.gauge("mxl_ringbuffer_headindex", "Last committed grain index", labels);
        _flowWriterActive = &metrics.gauge("mxl_flow_writer_active", "Flow writer active (0/1)", labels);
        _commitLatency = &metrics.histogram("mxl_flow_grain_commit_latency_seconds", "Grain open→commit latency", ops::Registry::latencyBuckets(),
            labels);
        _callbackDuration =
            &metrics.histogram("mxl_callback_duration_seconds", "DeckLink callback duration", ops::Registry::latencyBuckets(), labels);
    }

    InputChannel::~InputChannel()
    {
        stop();
    }

    void InputChannel::setState(State s)
    {
        auto const prev = _status.state.exchange(s);
        _stateGauge->set(static_cast<double>(static_cast<int>(s)));
        if (prev != s)
        {
            log::info("channel_state_changed",
                {
                    {"channel_index", _cfg.index},
                    {"channel_label", _cfg.label},
                    {"direction", "input"},
                    {"from", stateName(prev)},
                    {"to", stateName(s)},
                });
        }
    }

    void InputChannel::start()
    {
        _running.store(true);
        _supervisor = std::thread([this] {
            util::setThreadName("in-ch" + std::to_string(_cfg.index));
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

    void InputChannel::stop()
    {
        _running.store(false);
        if (_supervisor.joinable())
        {
            _supervisor.join();
        }
        tearDownStreaming();
    }

    void InputChannel::supervisorLoop()
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

            // Pending auto-detect format change (§3.8) — executed here so the
            // SDK callback stays non-blocking.
            bool havePending = false;
            {
                std::lock_guard const lock{_formatChangeMutex};
                havePending = _pendingFormatChange.has_value();
            }
            if (havePending)
            {
                performFormatChange();
            }

            // §3.6: reset cycle after prolonged signal loss.
            auto const lossSince = _signalLossSinceTai.load();
            if (lossSince != 0)
            {
                auto const now = util::taiNowNs();
                if (now - lossSince > static_cast<std::uint64_t>(_globalCfg.signalLossTimeoutS) * 1'000'000'000ULL)
                {
                    log::warn("signal_loss_stream_reset",
                        {
                            {"channel_index", _cfg.index},
                            {"channel_label", _cfg.label},
                            {"timeout_s", _globalCfg.signalLossTimeoutS},
                        });
                    _capture->resetStreams();
                    _signalLossSinceTai.store(util::taiNowNs());
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    bool InputChannel::bringUp()
    {
        setState(State::Init);
        try
        {
            _currentMode = _cfg.isAutoMode() ? autoStartMode() : *_cfg.videoMode;

            // §3.10 step 5: verify the mode (explicit modes only; §3.2).
            if (!_cfg.isAutoMode() && !_subDevice.supportsMode(_currentMode, config::Direction::Input))
            {
                log::error("video_mode_not_supported",
                    {
                        {"channel_index", _cfg.index},
                        {"video_mode", _currentMode.name},
                    });
                return false;
            }

            // §3.10 step 6: MXL writers before DeckLink enable.
            createWriters(_currentMode, _cfg.videoFlowId, _cfg.audioFlowId, _cfg.ancFlowId);

            // §3.10 steps 7–9.
            _capture = _subDevice.openCapture();
            if (!_capture)
            {
                log::error("open_capture_failed", {{"channel_index", _cfg.index}});
                destroyWriters();
                return false;
            }

            dl::ICaptureSession::Params params;
            params.mode = _currentMode;
            params.enableFormatDetection = _cfg.isAutoMode();
            params.pixelFormat = _cfg.pixelFormat;
            params.audioEnable = _cfg.audioEnable;
            params.audioChannelCount = _cfg.audioChannelCount;
            params.audioSampleType = _cfg.audioSampleType;
            params.collectAncPackets = _cfg.ancEnable;

            dl::CaptureCallbacks callbacks;
            callbacks.onFrame = [this](dl::VideoFrameView const& v, dl::AudioPacketView const* a) {
                handleFrame(v, a);
            };
            callbacks.onFormatChanged = [this](dl::FormatChange const& fc) {
                handleFormatChange(fc);
            };

            if (auto const err = _capture->enable(params, std::move(callbacks)))
            {
                log::error("capture_enable_failed", {{"channel_index", _cfg.index}, {"details", *err}});
                _capture.reset();
                destroyWriters();
                return false;
            }

            // §3.5: initial hardware-clock calibration.
            if (_useHardwareTimestamps.load())
            {
                if (auto const hwNow = _capture->hardwareClockNowNs())
                {
                    _hwCalibrator.calibrate(*hwNow, util::taiNowNs());
                }
                else
                {
                    log::warn("hardware_timestamp_unavailable",
                        {
                            {"channel_index", _cfg.index},
                            {"details", "falling back to host CLOCK_TAI timestamps"},
                        });
                    _useHardwareTimestamps.store(false);
                }
            }

            if (auto const err = _capture->start())
            {
                log::error("capture_start_failed", {{"channel_index", _cfg.index}, {"details", *err}});
                _capture->disable();
                _capture.reset();
                destroyWriters();
                return false;
            }

            _audioIndexValid = false;
            _streamingUp.store(true);
            setState(State::Healthy);
            log::info("input_channel_started",
                {
                    {"channel_index", _cfg.index},
                    {"channel_label", _cfg.label},
                    {"video_mode", _currentMode.name},
                    {"flow_id", _status.activeVideoFlowId()},
                });
            return true;
        }
        catch (std::exception const& e)
        {
            log::error("input_channel_bringup_exception", {{"channel_index", _cfg.index}, {"details", e.what()}});
            _capture.reset();
            destroyWriters();
            return false;
        }
    }

    void InputChannel::tearDownStreaming()
    {
        _streamingUp.store(false);
        if (_capture)
        {
            _capture->stop();
            _capture->disable();
            _capture.reset();
        }
        destroyWriters();
    }

    void InputChannel::createWriters(config::VideoMode const& mode, util::Uuid const& videoId, std::optional<util::Uuid> const& audioId,
        std::optional<util::Uuid> const& ancId)
    {
        std::lock_guard const lock{_writerMutex};

        mxlbridge::VideoFlowParams vp;
        vp.id = videoId;
        vp.label = _cfg.videoFlowLabel.empty() ? _cardIdLabel + "-ch" + std::to_string(_cfg.index) + "-video" : _cfg.videoFlowLabel;
        vp.description = "mxl-decklink input channel " + std::to_string(_cfg.index) + " video";
        vp.groupHint = _cfg.groupHint + ":Video";
        vp.sourceId = _cfg.sourceId;
        vp.deviceId = _cfg.deviceId;
        vp.width = mode.width;
        vp.height = mode.height;
        vp.rateNumerator = mode.rateNumerator;
        vp.rateDenominator = mode.rateDenominator;
        vp.interlaced = mode.interlaced;
        vp.withAlpha = _cfg.pixelFormat == config::PixelFormat::YUVA10;
        _videoWriter = std::make_unique<mxlbridge::VideoWriter>(_domain, vp, _cfg.commitBatchHintVideo);

        // §4.2 deviation note (IMPLEMENTATION_PLAN.md §3): the actual ring
        // depth is governed by the domain history duration in MXL v1.0.1.
        if (static_cast<int>(_videoWriter->actualGrainCount()) != _cfg.effectiveGrainCount())
        {
            log::warn("grain_count_differs",
                {
                    {"channel_index", _cfg.index},
                    {"requested", _cfg.effectiveGrainCount()},
                    {"actual", _videoWriter->actualGrainCount()},
                    {"details", "ring depth is set by the domain history duration (options.json) in MXL v1.0.1"},
                });
        }

        if (_cfg.audioEnable && audioId)
        {
            mxlbridge::AudioFlowParams ap;
            ap.id = *audioId;
            ap.label = _cfg.audioFlowLabel.empty() ? _cardIdLabel + "-ch" + std::to_string(_cfg.index) + "-audio" : _cfg.audioFlowLabel;
            ap.description = "mxl-decklink input channel " + std::to_string(_cfg.index) + " audio";
            ap.groupHint = _cfg.groupHint + ":Audio";
            ap.sourceId = _cfg.sourceId;
            ap.deviceId = _cfg.deviceId;
            ap.channelCount = static_cast<std::uint32_t>(_cfg.audioChannelCount);
            _audioWriter = std::make_unique<mxlbridge::AudioWriter>(_domain, ap, _cfg.commitBatchHintAudio);
        }

        if (_cfg.ancEnable && ancId)
        {
            mxlbridge::AncFlowParams np;
            np.id = *ancId;
            np.label = _cardIdLabel + "-ch" + std::to_string(_cfg.index) + "-anc";
            np.description = "mxl-decklink input channel " + std::to_string(_cfg.index) + " ancillary data";
            np.groupHint = _cfg.groupHint + ":ANC";
            np.sourceId = _cfg.sourceId;
            np.deviceId = _cfg.deviceId;
            np.rateNumerator = mode.rateNumerator;
            np.rateDenominator = mode.rateDenominator;
            _ancWriter = std::make_unique<mxlbridge::AncWriter>(_domain, np);
        }

        _writersValid.store(true);
        _flowWriterActive->set(1);
    }

    void InputChannel::destroyWriters()
    {
        _writersValid.store(false);
        std::lock_guard const lock{_writerMutex};
        _videoWriter.reset();
        _audioWriter.reset();
        _ancWriter.reset();
        _flowWriterActive->set(0);
    }

    std::uint64_t InputChannel::frameTimestampTai(dl::VideoFrameView const& video)
    {
        if (_useHardwareTimestamps.load() && video.hasHardwareTimestamp && _hwCalibrator.isCalibrated())
        {
            return _hwCalibrator.toTai(video.hardwareTimestampNs);
        }
        return util::taiNowNs();
    }

    void InputChannel::handleFrame(dl::VideoFrameView const& video, dl::AudioPacketView const* audio)
    {
        auto const callbackStart = util::taiNowNs();

        // §3.6: signal loss → standby, no grain committed (ring gap).
        if (video.hasNoInputSource)
        {
            std::uint64_t expected = 0;
            if (_signalLossSinceTai.compare_exchange_strong(expected, callbackStart))
            {
                _signalLostTotal->inc();
                log::warn("signal_lost", {{"channel_index", _cfg.index}, {"channel_label", _cfg.label}});
                setState(State::Degraded);
            }
            _status.signalLocked.store(false);
            return;
        }
        if (_signalLossSinceTai.exchange(0) != 0)
        {
            log::info("signal_restored", {{"channel_index", _cfg.index}, {"channel_label", _cfg.label}});
            _audioIndexValid = false;
        }
        _status.signalLocked.store(true);
        if (_status.state.load() == State::Degraded)
        {
            setState(State::Healthy);
        }

        if (!_writersValid.load(std::memory_order_acquire))
        {
            return; // format change in progress (§3.8)
        }
        // The hot path must never block behind the format-change path.
        std::unique_lock lock{_writerMutex, std::try_to_lock};
        if (!lock.owns_lock() || !_videoWriter)
        {
            return;
        }

        auto const tai = frameTimestampTai(video);
        auto const rate = _videoWriter->grainRate();
        auto const grainIndex = ::mxlTimestampToIndex(&rate, tai);

        // Video grain (§2.3): one memcpy for v210; expansion for 8-bit.
        auto const commitStart = util::taiNowNs();
        mxlStatus status;
        if (_cfg.pixelFormat == config::PixelFormat::YUV8)
        {
            auto const dstRowBytes = util::v210RowBytes(video.width);
            _conversionBuffer.resize(static_cast<std::size_t>(dstRowBytes) * video.height);
            util::expandUyvyToV210(video.bytes, video.rowBytes, _conversionBuffer.data(), dstRowBytes, video.width, video.height);
            status = _videoWriter->writeFrame(grainIndex, _conversionBuffer.data(), _conversionBuffer.size());
        }
        else
        {
            status = _videoWriter->writeFrame(grainIndex, video.bytes, video.rowBytes * video.height);
        }

        if (status == MXL_STATUS_OK)
        {
            auto const commitEnd = util::taiNowNs();
            _commitLatency->observe(static_cast<double>(commitEnd - commitStart) / 1e9);
            _grainsCommitted->inc();
            _grainsWrittenBytes->inc(static_cast<double>(_videoWriter->grainPayloadSize()));
            _status.grainsCommitted.fetch_add(1);
            _status.lastFrameTaiNs.store(tai);
            _headIndexGauge->set(static_cast<double>(grainIndex));
        }
        else
        {
            _framesDropped->inc();
            _status.framesDropped.fetch_add(1);
        }
        _framesTotal->inc();
        _status.framesTotal.fetch_add(1);

        // Audio (§3.4): deinterleave + float conversion into the ring.
        if (audio != nullptr && _audioWriter)
        {
            mxlRational const audioRate{48000, 1};
            auto const nowEnd = ::mxlTimestampToIndex(&audioRate, tai);
            if (!_audioIndexValid)
            {
                _nextAudioEndIndex = nowEnd;
                _audioIndexValid = true;
            }
            else
            {
                _nextAudioEndIndex += audio->sampleFrames;
                // Resync when drift against the TAI grid exceeds 100 ms
                // (§3.4: no rate compensation, but discontinuities must heal).
                auto const drift = _nextAudioEndIndex > nowEnd ? _nextAudioEndIndex - nowEnd : nowEnd - _nextAudioEndIndex;
                if (drift > 4800)
                {
                    log::warn("audio_index_resync",
                        {
                            {"channel_index", _cfg.index},
                            {"drift_samples", drift},
                        });
                    _nextAudioEndIndex = nowEnd;
                }
            }
            auto const audioStatus = _audioWriter->writeSamples(_nextAudioEndIndex, audio->bytes, audio->sampleFrames,
                static_cast<std::size_t>(_cfg.audioChannelCount), _cfg.audioSampleType);
            if (audioStatus != MXL_STATUS_OK)
            {
                log::debug("audio_write_failed", {{"channel_index", _cfg.index}, {"status", static_cast<int>(audioStatus)}});
            }
        }

        // ANC (§2.3): RFC 8331 grain aligned with the video grain index.
        if (_cfg.ancEnable && _ancWriter && !video.ancPackets.empty())
        {
            auto const ancStatus = _ancWriter->writePackets(grainIndex, video.ancPackets);
            if (ancStatus != MXL_STATUS_OK)
            {
                log::debug("anc_write_failed", {{"channel_index", _cfg.index}, {"status", static_cast<int>(ancStatus)}});
            }
        }

        _callbackDuration->observe(static_cast<double>(util::taiNowNs() - callbackStart) / 1e9);
    }

    void InputChannel::handleFormatChange(dl::FormatChange const& fc)
    {
        log::info("format_change_detected",
            {
                {"channel_index", _cfg.index},
                {"channel_label", _cfg.label},
                {"new_mode", fc.newMode.name},
            });
        _formatChangesTotal->inc();
        // Grains stop flowing immediately; the supervisor thread executes the
        // flow swap (§3.8) outside the SDK callback.
        _writersValid.store(false, std::memory_order_release);
        std::lock_guard const lock{_formatChangeMutex};
        _pendingFormatChange = fc;
    }

    void InputChannel::performFormatChange()
    {
        dl::FormatChange fc;
        {
            std::lock_guard const lock{_formatChangeMutex};
            if (!_pendingFormatChange)
            {
                return;
            }
            fc = *_pendingFormatChange;
            _pendingFormatChange.reset();
        }

        setState(State::Degraded);

        // §3.2: re-enable sequence on the capture side.
        if (auto const err = _capture->changeMode(fc.newMode))
        {
            log::error("format_change_reenable_failed", {{"channel_index", _cfg.index}, {"details", *err}});
            tearDownStreaming();
            return; // supervisor loop re-runs bringUp with backoff
        }
        _currentMode = fc.newMode;

        // §3.8: release writers, grace period, then a new flow UUID.
        destroyWriters();
        std::this_thread::sleep_for(kFormatChangeGrace);

        ++_formatChangeCounter;
        auto const signature = fc.newMode.name + "#" + std::to_string(_formatChangeCounter);
        auto const newVideoId = util::deriveUuid(_cfg.videoFlowId, signature);
        auto const newAudioId = _cfg.audioFlowId ? std::make_optional(util::deriveUuid(*_cfg.audioFlowId, signature)) : std::nullopt;
        auto const newAncId = _cfg.ancFlowId ? std::make_optional(util::deriveUuid(*_cfg.ancFlowId, signature)) : std::nullopt;

        try
        {
            createWriters(fc.newMode, newVideoId, newAudioId, newAncId);
        }
        catch (std::exception const& e)
        {
            log::error("format_change_writer_recreate_failed", {{"channel_index", _cfg.index}, {"details", e.what()}});
            tearDownStreaming();
            return;
        }

        _audioIndexValid = false;
        _status.setActiveVideoFlowId(newVideoId.toString());

        // §3.8: the new flow UUID is announced via a structured log event and
        // the mxl_active_video_flow_id info metric (see health.cpp).
        log::info("video_flow_replaced",
            {
                {"channel_index", _cfg.index},
                {"channel_label", _cfg.label},
                {"video_mode", fc.newMode.name},
                {"flow_id", newVideoId.toString()},
                {"previous_flow_id", _cfg.videoFlowId.toString()},
            });
        setState(State::Healthy);
    }

    void InputChannel::housekeeping()
    {
        if (!_streamingUp.load() || !_capture)
        {
            return;
        }
        _signalLockGauge->set(_capture->signalLocked() ? 1.0 : 0.0);

        // §3.5: rolling recalibration every 60 s with a 1 ms gate.
        if (_useHardwareTimestamps.load())
        {
            auto const now = util::taiNowNs();
            if (_hwCalibrator.needsRecalibration(now))
            {
                if (auto const hwNow = _capture->hardwareClockNowNs())
                {
                    _hwCalibrator.calibrate(*hwNow, util::taiNowNs());
                }
            }
        }
    }
}
