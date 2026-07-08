// SPDX-License-Identifier: MIT
// Input channel: DeckLink capture → MXL flows (SPECIFICATION.md §2.3, §3.6,
// §3.8).
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "channel/state.hpp"
#include "config/config.hpp"
#include "decklink/device.hpp"
#include "mxlbridge/ancwriter.hpp"
#include "mxlbridge/audiowriter.hpp"
#include "mxlbridge/domain.hpp"
#include "mxlbridge/videowriter.hpp"
#include "ops/metrics.hpp"
#include "util/taiclock.hpp"

namespace mxldl::channel
{
    class InputChannel
    {
    public:
        InputChannel(config::Config const& globalCfg, config::ChannelConfig const& chCfg, dl::ISubDevice& subDevice, mxlbridge::Domain& domain,
            ops::Registry& metrics, std::string cardIdLabel);
        ~InputChannel();

        InputChannel(InputChannel const&) = delete;
        InputChannel& operator=(InputChannel const&) = delete;

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

        /// Housekeeping poll (§2.5): updates signal-lock gauge and performs
        /// the hardware-clock rolling recalibration (§3.5).
        void housekeeping();

    private:
        void supervisorLoop();
        bool bringUp();
        void tearDownStreaming();
        void handleFrame(dl::VideoFrameView const& video, dl::AudioPacketView const* audio);
        void handleFormatChange(dl::FormatChange const& fc);
        void performFormatChange();
        std::uint64_t frameTimestampTai(dl::VideoFrameView const& video);
        void createWriters(config::VideoMode const& mode, util::Uuid const& videoId, std::optional<util::Uuid> const& audioId,
            std::optional<util::Uuid> const& ancId);
        void destroyWriters();

        config::Config const& _globalCfg;
        config::ChannelConfig _cfg;
        dl::ISubDevice& _subDevice;
        mxlbridge::Domain& _domain;
        std::string _cardIdLabel;

        Status _status;
        std::unique_ptr<dl::ICaptureSession> _capture;

        // Writers guarded by _writerMutex against the format-change path; the
        // hot path takes it uncontended (try_lock) — see handleFrame.
        std::mutex _writerMutex;
        std::unique_ptr<mxlbridge::VideoWriter> _videoWriter;
        std::unique_ptr<mxlbridge::AudioWriter> _audioWriter;
        std::unique_ptr<mxlbridge::AncWriter> _ancWriter;
        std::atomic<bool> _writersValid{false};

        config::VideoMode _currentMode{};
        util::HardwareClockCalibrator _hwCalibrator;
        std::atomic<bool> _useHardwareTimestamps{false};

        // Audio index continuity (§3.4).
        std::uint64_t _nextAudioEndIndex = 0;
        bool _audioIndexValid = false;

        // Conversion scratch (8-bit expansion, §3.3).
        std::vector<std::uint8_t> _conversionBuffer;

        // Signal-loss handling (§3.6).
        std::atomic<std::uint64_t> _signalLossSinceTai{0};

        // Format change (§3.8).
        std::mutex _formatChangeMutex;
        std::optional<dl::FormatChange> _pendingFormatChange;
        int _formatChangeCounter = 0;

        std::thread _supervisor;
        std::atomic<bool> _running{false};
        std::atomic<bool> _streamingUp{false};

        // Metrics (§7.3).
        ops::Counter* _framesTotal = nullptr;
        ops::Counter* _framesDropped = nullptr;
        ops::Counter* _signalLostTotal = nullptr;
        ops::Counter* _formatChangesTotal = nullptr;
        ops::Counter* _reconnectTotal = nullptr;
        ops::Counter* _grainsCommitted = nullptr;
        ops::Counter* _grainsWrittenBytes = nullptr;
        ops::Gauge* _stateGauge = nullptr;
        ops::Gauge* _signalLockGauge = nullptr;
        ops::Gauge* _headIndexGauge = nullptr;
        ops::Gauge* _flowWriterActive = nullptr;
        ops::Histogram* _commitLatency = nullptr;
        ops::Histogram* _callbackDuration = nullptr;

        void setState(State s);
    };
}
