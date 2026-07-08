// SPDX-License-Identifier: MIT
// Production DeckLink backend on the Blackmagic COM interfaces. The API
// dispatch shim (third_party/decklink/DeckLinkAPIDispatch.cpp) dlopens
// libDeckLinkAPI.so at runtime, so this backend compiles everywhere and fails
// gracefully at startup when Desktop Video is absent.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include <DeckLinkAPI.h>

#include "decklink/device.hpp"
#include "util/logging.hpp"

namespace mxldl::dl
{
    namespace
    {
        constexpr BMDVideoConnection kAnyConnection = 0; // bmdVideoConnectionUnspecified

        /// Minimal COM smart pointer.
        template<typename T>
        class ComPtr
        {
        public:
            ComPtr() = default;

            explicit ComPtr(T* p)
                : _p(p)
            {}

            ComPtr(ComPtr const& other)
                : _p(other._p)
            {
                if (_p != nullptr)
                {
                    _p->AddRef();
                }
            }

            ComPtr(ComPtr&& other) noexcept
                : _p(other._p)
            {
                other._p = nullptr;
            }

            ComPtr& operator=(ComPtr other) noexcept
            {
                std::swap(_p, other._p);
                return *this;
            }

            ~ComPtr()
            {
                if (_p != nullptr)
                {
                    _p->Release();
                }
            }

            T* get() const
            {
                return _p;
            }

            T* operator->() const
            {
                return _p;
            }

            explicit operator bool() const
            {
                return _p != nullptr;
            }

            /// For out-params: releases the current pointer first.
            T** receive()
            {
                if (_p != nullptr)
                {
                    _p->Release();
                    _p = nullptr;
                }
                return &_p;
            }

            template<typename U>
            ComPtr<U> queryInterface(REFIID iid) const
            {
                void* out = nullptr;
                if (_p != nullptr && _p->QueryInterface(iid, &out) == S_OK)
                {
                    return ComPtr<U>(static_cast<U*>(out));
                }
                return ComPtr<U>();
            }

        private:
            T* _p = nullptr;
        };

        BMDPixelFormat toBmdPixelFormat(config::PixelFormat f)
        {
            switch (f)
            {
                case config::PixelFormat::YUV10: return bmdFormat10BitYUV;
                case config::PixelFormat::YUVA10: return bmdFormat10BitYUV; // fill plane; key composed in software
                case config::PixelFormat::YUV8: return bmdFormat8BitYUV;
            }
            return bmdFormat10BitYUV;
        }

        BMDAudioSampleType toBmdSampleType(config::AudioSampleType t)
        {
            return t == config::AudioSampleType::Int32 ? bmdAudioSampleType32bitInteger : bmdAudioSampleType16bitInteger;
        }

        std::string hresultMessage(char const* what, HRESULT hr)
        {
            char buf[64];
            ::snprintf(buf, sizeof(buf), "%s failed (HRESULT 0x%08x)", what, static_cast<unsigned>(hr));
            return buf;
        }

        // ------------------------------------------------------------------
        // Capture
        // ------------------------------------------------------------------
        class SdkCaptureSession final
            : public ICaptureSession
            , public IDeckLinkInputCallback
        {
        public:
            SdkCaptureSession(ComPtr<IDeckLinkInput> input, ComPtr<IDeckLinkStatus> status)
                : _input(std::move(input))
                , _status(std::move(status))
            {}

            ~SdkCaptureSession() override
            {
                disable();
            }

            // --- ICaptureSession ------------------------------------------
            std::optional<std::string> enable(Params const& params, CaptureCallbacks callbacks) override
            {
                _params = params;
                _callbacks = std::move(callbacks);

                BMDVideoInputFlags flags = bmdVideoInputFlagDefault;
                if (params.enableFormatDetection)
                {
                    flags |= bmdVideoInputEnableFormatDetection;
                }
                auto hr = _input->EnableVideoInput(params.mode.bmdDisplayMode, toBmdPixelFormat(params.pixelFormat), flags);
                if (hr != S_OK)
                {
                    return hresultMessage("EnableVideoInput", hr);
                }
                if (params.audioEnable)
                {
                    hr = _input->EnableAudioInput(bmdAudioSampleRate48kHz, toBmdSampleType(params.audioSampleType),
                        static_cast<uint32_t>(params.audioChannelCount));
                    if (hr != S_OK)
                    {
                        _input->DisableVideoInput();
                        return hresultMessage("EnableAudioInput", hr);
                    }
                }
                hr = _input->SetCallback(this);
                if (hr != S_OK)
                {
                    disable();
                    return hresultMessage("SetCallback", hr);
                }
                return std::nullopt;
            }

            std::optional<std::string> start() override
            {
                auto const hr = _input->StartStreams();
                if (hr != S_OK)
                {
                    return hresultMessage("StartStreams", hr);
                }
                return std::nullopt;
            }

            void stop() override
            {
                _input->StopStreams();
                _input->FlushStreams();
            }

            void resetStreams() override
            {
                // §3.6: Blackmagic-recommended reset idiom.
                _input->StopStreams();
                _input->FlushStreams();
                _input->StartStreams();
            }

            std::optional<std::string> changeMode(config::VideoMode const& mode) override
            {
                // §3.2 re-enable sequence.
                _input->PauseStreams();
                BMDVideoInputFlags flags = bmdVideoInputFlagDefault;
                if (_params.enableFormatDetection)
                {
                    flags |= bmdVideoInputEnableFormatDetection;
                }
                auto const hr = _input->EnableVideoInput(mode.bmdDisplayMode, toBmdPixelFormat(_params.pixelFormat), flags);
                if (hr != S_OK)
                {
                    return hresultMessage("EnableVideoInput (mode change)", hr);
                }
                _input->FlushStreams();
                _input->StartStreams();
                _params.mode = mode;
                return std::nullopt;
            }

            void disable() override
            {
                _input->StopStreams();
                _input->SetCallback(nullptr);
                _input->DisableVideoInput();
                _input->DisableAudioInput();
            }

            bool signalLocked() override
            {
                if (!_status)
                {
                    return false;
                }
                bool locked = false;
                return _status->GetFlag(bmdDeckLinkStatusVideoInputSignalLocked, &locked) == S_OK && locked;
            }

            std::optional<std::uint64_t> hardwareClockNowNs() override
            {
                BMDTimeValue hwTime = 0;
                BMDTimeValue timeInFrame = 0;
                BMDTimeValue ticksPerFrame = 0;
                if (_input->GetHardwareReferenceClock(1'000'000'000LL, &hwTime, &timeInFrame, &ticksPerFrame) != S_OK)
                {
                    return std::nullopt;
                }
                return static_cast<std::uint64_t>(hwTime);
            }

            // --- IDeckLinkInputCallback -----------------------------------
            HRESULT VideoInputFormatChanged(BMDVideoInputFormatChangedEvents /*events*/, IDeckLinkDisplayMode* newDisplayMode,
                BMDDetectedVideoInputFormatFlags detectedSignalFlags) override
            {
                if (newDisplayMode == nullptr || !_callbacks.onFormatChanged)
                {
                    return S_OK;
                }
                auto const mode = config::lookupVideoModeByBmd(newDisplayMode->GetDisplayMode());
                if (!mode)
                {
                    log::warn("format_change_unknown_mode", {{"bmd_mode", static_cast<std::uint64_t>(newDisplayMode->GetDisplayMode())}});
                    return S_OK;
                }
                FormatChange fc;
                fc.newMode = *mode;
                fc.is10Bit = (detectedSignalFlags & bmdDetectedVideoInput8BitDepth) == 0;
                _callbacks.onFormatChanged(fc);
                return S_OK;
            }

            HRESULT VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame, IDeckLinkAudioInputPacket* audioPacket) override
            {
                if (!_callbacks.onFrame)
                {
                    return S_OK;
                }

                VideoFrameView view;
                AudioPacketView audioView;
                AudioPacketView const* audioPtr = nullptr;

                if (videoFrame != nullptr)
                {
                    void* bytes = nullptr;
                    if (videoFrame->GetBytes(&bytes) == S_OK)
                    {
                        view.bytes = static_cast<std::uint8_t const*>(bytes);
                    }
                    view.rowBytes = static_cast<std::size_t>(videoFrame->GetRowBytes());
                    view.width = static_cast<std::uint32_t>(videoFrame->GetWidth());
                    view.height = static_cast<std::uint32_t>(videoFrame->GetHeight());
                    view.hasNoInputSource = (videoFrame->GetFlags() & bmdFrameHasNoInputSource) != 0;

                    BMDTimeValue t = 0;
                    BMDTimeValue d = 0;
                    if (videoFrame->GetHardwareReferenceTimestamp(1'000'000'000LL, &t, &d) == S_OK)
                    {
                        view.hasHardwareTimestamp = true;
                        view.hardwareTimestampNs = static_cast<std::uint64_t>(t);
                    }

                    if (_params.collectAncPackets && !view.hasNoInputSource)
                    {
                        collectAnc(videoFrame, view.ancPackets);
                    }
                }
                else
                {
                    view.hasNoInputSource = true;
                }

                if (audioPacket != nullptr)
                {
                    void* bytes = nullptr;
                    if (audioPacket->GetBytes(&bytes) == S_OK)
                    {
                        audioView.bytes = bytes;
                        audioView.sampleFrames = static_cast<std::size_t>(audioPacket->GetSampleFrameCount());
                        audioPtr = &audioView;
                    }
                }

                _callbacks.onFrame(view, audioPtr);
                return S_OK;
            }

            // --- IUnknown --------------------------------------------------
            HRESULT QueryInterface(REFIID /*iid*/, LPVOID* ppv) override
            {
                *ppv = nullptr;
                return E_NOINTERFACE;
            }

            ULONG AddRef() override
            {
                return ++_refCount;
            }

            ULONG Release() override
            {
                // Lifetime is owned by unique_ptr on the channel side; the SDK
                // ref-count is tracked but never deletes.
                return --_refCount;
            }

        private:
            static void collectAnc(IDeckLinkVideoInputFrame* frame, std::vector<util::AncPacket>& out)
            {
                void* itf = nullptr;
                if (frame->QueryInterface(IID_IDeckLinkVideoFrameAncillaryPackets, &itf) != S_OK)
                {
                    return;
                }
                ComPtr<IDeckLinkVideoFrameAncillaryPackets> packets{static_cast<IDeckLinkVideoFrameAncillaryPackets*>(itf)};
                ComPtr<IDeckLinkAncillaryPacketIterator> iter;
                if (packets->GetPacketIterator(iter.receive()) != S_OK || !iter)
                {
                    return;
                }
                ComPtr<IDeckLinkAncillaryPacket> pkt;
                while (iter->Next(pkt.receive()) == S_OK && pkt)
                {
                    util::AncPacket p;
                    p.did = pkt->GetDID();
                    p.sdid = pkt->GetSDID();
                    p.lineNumber = pkt->GetLineNumber();
                    p.dataStreamIndex = pkt->GetDataStreamIndex();
                    void const* data = nullptr;
                    uint32_t size = 0;
                    if (pkt->GetBytes(bmdAncillaryPacketFormatUInt8, &data, &size) == S_OK && data != nullptr)
                    {
                        auto const* bytes = static_cast<std::uint8_t const*>(data);
                        p.userData.assign(bytes, bytes + size);
                    }
                    out.push_back(std::move(p));
                }
            }

            ComPtr<IDeckLinkInput> _input;
            ComPtr<IDeckLinkStatus> _status;
            Params _params;
            CaptureCallbacks _callbacks;
            std::atomic<ULONG> _refCount{1};
        };

        // ------------------------------------------------------------------
        // Playback
        // ------------------------------------------------------------------
        class SdkPlaybackSession final
            : public IPlaybackSession
            , public IDeckLinkVideoOutputCallback
            , public IDeckLinkAudioOutputCallback
        {
        public:
            SdkPlaybackSession(ComPtr<IDeckLinkOutput> output, ComPtr<IDeckLinkStatus> status)
                : _output(std::move(output))
                , _status(std::move(status))
            {}

            ~SdkPlaybackSession() override
            {
                disable();
            }

            // --- IPlaybackSession -----------------------------------------
            std::optional<std::string> enable(Params const& params, PlaybackCallbacks callbacks) override
            {
                _params = params;
                _callbacks = std::move(callbacks);

                auto hr = _output->EnableVideoOutput(params.mode.bmdDisplayMode, bmdVideoOutputFlagDefault);
                if (hr != S_OK)
                {
                    return hresultMessage("EnableVideoOutput", hr);
                }
                if (params.audioEnable)
                {
                    hr = _output->EnableAudioOutput(bmdAudioSampleRate48kHz, toBmdSampleType(params.audioSampleType),
                        static_cast<uint32_t>(params.audioChannelCount), bmdAudioOutputStreamContinuous);
                    if (hr != S_OK)
                    {
                        _output->DisableVideoOutput();
                        return hresultMessage("EnableAudioOutput", hr);
                    }
                    _output->SetAudioCallback(this);
                }
                _output->SetScheduledFrameCompletionCallback(this);
                return std::nullopt;
            }

            std::optional<std::uint64_t> scheduleFrame(std::uint8_t const* bytes, std::size_t size, std::int64_t displayTime,
                std::int64_t displayDuration, std::int64_t timeScale) override
            {
                ComPtr<IDeckLinkMutableVideoFrame> frame = acquireFrame();
                if (!frame)
                {
                    return std::nullopt;
                }
                void* dst = nullptr;
                if (frame->GetBytes(&dst) != S_OK)
                {
                    return std::nullopt;
                }
                std::size_t const cap = static_cast<std::size_t>(frame->GetRowBytes()) * static_cast<std::size_t>(frame->GetHeight());
                std::memcpy(dst, bytes, size < cap ? size : cap);

                std::uint64_t frameId = 0;
                {
                    std::lock_guard const lock{_mutex};
                    frameId = _nextFrameId++;
                    _inFlight[frame.get()] = frameId;
                }
                if (_output->ScheduleVideoFrame(frame.get(), displayTime, displayDuration, timeScale) != S_OK)
                {
                    std::lock_guard const lock{_mutex};
                    _inFlight.erase(frame.get());
                    _framePool.push_back(frame);
                    return std::nullopt;
                }
                // The device holds its own reference while the frame is queued;
                // keep ours in _inFlight via the pool round-trip on completion.
                {
                    std::lock_guard const lock{_mutex};
                    _scheduled.push_back(frame);
                }
                return frameId;
            }

            std::optional<std::string> scheduleAudio(void const* interleavedPcm, std::uint32_t sampleFrames) override
            {
                uint32_t written = 0;
                auto const hr =
                    _output->ScheduleAudioSamples(const_cast<void*>(interleavedPcm), sampleFrames, 0, 0, &written);
                if (hr != S_OK)
                {
                    return hresultMessage("ScheduleAudioSamples", hr);
                }
                return std::nullopt;
            }

            std::optional<std::string> beginAudioPreroll() override
            {
                if (!_params.audioEnable)
                {
                    return std::nullopt;
                }
                auto const hr = _output->BeginAudioPreroll();
                if (hr != S_OK)
                {
                    return hresultMessage("BeginAudioPreroll", hr);
                }
                return std::nullopt;
            }

            std::optional<std::string> endAudioPreroll() override
            {
                if (!_params.audioEnable)
                {
                    return std::nullopt;
                }
                auto const hr = _output->EndAudioPreroll();
                if (hr != S_OK)
                {
                    return hresultMessage("EndAudioPreroll", hr);
                }
                return std::nullopt;
            }

            std::optional<std::string> startPlayback(std::int64_t startTime, std::int64_t timeScale) override
            {
                auto const hr = _output->StartScheduledPlayback(startTime, timeScale, 1.0);
                if (hr != S_OK)
                {
                    return hresultMessage("StartScheduledPlayback", hr);
                }
                return std::nullopt;
            }

            void stopPlayback() override
            {
                BMDTimeValue actual = 0;
                _output->StopScheduledPlayback(0, &actual, 1'000'000'000LL);
                _output->FlushBufferedAudioSamples();
            }

            void disable() override
            {
                stopPlayback();
                _output->SetScheduledFrameCompletionCallback(nullptr);
                _output->SetAudioCallback(nullptr);
                _output->DisableVideoOutput();
                _output->DisableAudioOutput();
                std::lock_guard const lock{_mutex};
                _scheduled.clear();
                _framePool.clear();
                _inFlight.clear();
            }

            std::uint32_t bufferedVideoFrames() override
            {
                uint32_t n = 0;
                _output->GetBufferedVideoFrameCount(&n);
                return n;
            }

            std::uint32_t bufferedAudioFrames() override
            {
                uint32_t n = 0;
                _output->GetBufferedAudioSampleFrameCount(&n);
                return n;
            }

            bool referenceLocked() override
            {
                if (!_status)
                {
                    return false;
                }
                bool locked = false;
                return _status->GetFlag(bmdDeckLinkStatusReferenceSignalLocked, &locked) == S_OK && locked;
            }

            // --- IDeckLinkVideoOutputCallback -----------------------------
            HRESULT ScheduledFrameCompleted(IDeckLinkVideoFrame* completedFrame, BMDOutputFrameCompletionResult result) override
            {
                std::uint64_t frameId = 0;
                {
                    std::lock_guard const lock{_mutex};
                    auto const it = _inFlight.find(completedFrame);
                    if (it != _inFlight.end())
                    {
                        frameId = it->second;
                        _inFlight.erase(it);
                    }
                    // Return the frame to the pool and drop it from the
                    // scheduled list.
                    for (auto sit = _scheduled.begin(); sit != _scheduled.end(); ++sit)
                    {
                        if (sit->get() == completedFrame)
                        {
                            _framePool.push_back(*sit);
                            _scheduled.erase(sit);
                            break;
                        }
                    }
                }
                if (_callbacks.onFrameCompleted)
                {
                    CompletionResult r = CompletionResult::Completed;
                    switch (result)
                    {
                        case bmdOutputFrameDisplayedLate: r = CompletionResult::DisplayedLate; break;
                        case bmdOutputFrameDropped: r = CompletionResult::Dropped; break;
                        case bmdOutputFrameFlushed: r = CompletionResult::Flushed; break;
                        default: break;
                    }
                    _callbacks.onFrameCompleted(frameId, r);
                }
                return S_OK;
            }

            HRESULT ScheduledPlaybackHasStopped() override
            {
                if (_callbacks.onPlaybackStopped)
                {
                    _callbacks.onPlaybackStopped();
                }
                return S_OK;
            }

            // --- IDeckLinkAudioOutputCallback -----------------------------
            HRESULT RenderAudioSamples(bool preroll) override
            {
                if (_callbacks.onRenderAudio)
                {
                    _callbacks.onRenderAudio(preroll);
                }
                return S_OK;
            }

            // --- IUnknown --------------------------------------------------
            HRESULT QueryInterface(REFIID /*iid*/, LPVOID* ppv) override
            {
                *ppv = nullptr;
                return E_NOINTERFACE;
            }

            ULONG AddRef() override
            {
                return ++_refCount;
            }

            ULONG Release() override
            {
                return --_refCount;
            }

        private:
            ComPtr<IDeckLinkMutableVideoFrame> acquireFrame()
            {
                {
                    std::lock_guard const lock{_mutex};
                    if (!_framePool.empty())
                    {
                        auto f = _framePool.back();
                        _framePool.pop_back();
                        return f;
                    }
                }
                // v210 row stride (§3.3); UYVY is 2 bytes per pixel. The
                // trimmed interface headers predate RowBytesForPixelFormat,
                // and these formulas are normative for both formats.
                int32_t const actualRowBytes = _params.pixelFormat == config::PixelFormat::YUV8
                                                   ? static_cast<int32_t>(_params.mode.width * 2U)
                                                   : static_cast<int32_t>(((_params.mode.width + 47U) / 48U) * 128U);
                ComPtr<IDeckLinkMutableVideoFrame> frame;
                if (_output->CreateVideoFrame(static_cast<int32_t>(_params.mode.width), static_cast<int32_t>(_params.mode.height), actualRowBytes,
                        toBmdPixelFormat(_params.pixelFormat), bmdFrameFlagDefault, frame.receive()) != S_OK)
                {
                    return ComPtr<IDeckLinkMutableVideoFrame>();
                }
                return frame;
            }

            ComPtr<IDeckLinkOutput> _output;
            ComPtr<IDeckLinkStatus> _status;
            Params _params;
            PlaybackCallbacks _callbacks;
            std::mutex _mutex;
            std::vector<ComPtr<IDeckLinkMutableVideoFrame>> _framePool;
            std::vector<ComPtr<IDeckLinkMutableVideoFrame>> _scheduled;
            std::map<IDeckLinkVideoFrame*, std::uint64_t> _inFlight;
            std::uint64_t _nextFrameId = 1;
            std::atomic<ULONG> _refCount{1};
        };

        // ------------------------------------------------------------------
        // Sub-device / card / backend
        // ------------------------------------------------------------------
        class SdkSubDevice final : public ISubDevice
        {
        public:
            SdkSubDevice(ComPtr<IDeckLink> device, SubDeviceInfo info)
                : _device(std::move(device))
                , _info(std::move(info))
            {}

            [[nodiscard]] SubDeviceInfo const& info() const override
            {
                return _info;
            }

            bool supportsMode(config::VideoMode const& mode, config::Direction direction) override
            {
                if (direction == config::Direction::Input)
                {
                    auto input = _device.queryInterface<IDeckLinkInput>(IID_IDeckLinkInput);
                    if (!input)
                    {
                        return false;
                    }
                    BMDDisplayMode actual = bmdModeUnknown;
                    bool supported = false;
                    return input->DoesSupportVideoMode(kAnyConnection, mode.bmdDisplayMode, bmdFormat10BitYUV, bmdNoVideoInputConversion,
                               bmdSupportedVideoModeDefault, &actual, &supported) == S_OK &&
                           supported;
                }
                auto output = _device.queryInterface<IDeckLinkOutput>(IID_IDeckLinkOutput);
                if (!output)
                {
                    return false;
                }
                BMDDisplayMode actual = bmdModeUnknown;
                bool supported = false;
                return output->DoesSupportVideoMode(kAnyConnection, mode.bmdDisplayMode, bmdFormat10BitYUV, bmdNoVideoOutputConversion,
                           bmdSupportedVideoModeDefault, &actual, &supported) == S_OK &&
                       supported;
            }

            std::unique_ptr<ICaptureSession> openCapture() override
            {
                auto input = _device.queryInterface<IDeckLinkInput>(IID_IDeckLinkInput);
                if (!input)
                {
                    return nullptr;
                }
                auto status = _device.queryInterface<IDeckLinkStatus>(IID_IDeckLinkStatus);
                return std::make_unique<SdkCaptureSession>(std::move(input), std::move(status));
            }

            std::unique_ptr<IPlaybackSession> openPlayback() override
            {
                auto output = _device.queryInterface<IDeckLinkOutput>(IID_IDeckLinkOutput);
                if (!output)
                {
                    return nullptr;
                }
                auto status = _device.queryInterface<IDeckLinkStatus>(IID_IDeckLinkStatus);
                return std::make_unique<SdkPlaybackSession>(std::move(output), std::move(status));
            }

            [[nodiscard]] ComPtr<IDeckLink> const& raw() const
            {
                return _device;
            }

        private:
            ComPtr<IDeckLink> _device;
            SubDeviceInfo _info;
        };

        class SdkCard final
            : public ICard
            , public IDeckLinkProfileCallback
        {
        public:
            SdkCard(std::string displayName, std::uint32_t persistentId, std::vector<std::unique_ptr<SdkSubDevice>> subDevices)
                : _displayName(std::move(displayName))
                , _persistentId(persistentId)
                , _subDevices(std::move(subDevices))
            {}

            ~SdkCard() override
            {
                for (auto& mgr : _profileManagers)
                {
                    mgr->SetCallback(nullptr);
                }
            }

            [[nodiscard]] std::string const& displayName() const override
            {
                return _displayName;
            }

            [[nodiscard]] std::uint32_t persistentId() const override
            {
                return _persistentId;
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
                auto mgr = _subDevices.front()->raw().queryInterface<IDeckLinkProfileManager>(IID_IDeckLinkProfileManager);
                if (!mgr)
                {
                    // §3.9: profile-less cards (DeckLink IP 100G) ignore the setting.
                    log::info("card_profile_ignored",
                        {
                            {"card", _displayName},
                            {"details", "card has no profile manager; MXL_DECKLINK_CARD_PROFILE ignored"},
                        });
                    return std::nullopt;
                }

                BMDProfileID profileId = bmdProfileOneSubDeviceFullDuplex;
                switch (profile)
                {
                    case config::CardProfile::OneFullDuplex: profileId = bmdProfileOneSubDeviceFullDuplex; break;
                    case config::CardProfile::OneHalfDuplex: profileId = bmdProfileOneSubDeviceHalfDuplex; break;
                    case config::CardProfile::TwoHalfDuplex: profileId = bmdProfileTwoSubDevicesHalfDuplex; break;
                    case config::CardProfile::FourHalfDuplex: profileId = bmdProfileFourSubDevicesHalfDuplex; break;
                }

                ComPtr<IDeckLinkProfile> profileObj;
                auto hr = mgr->GetProfile(profileId, profileObj.receive());
                if (hr != S_OK || !profileObj)
                {
                    return hresultMessage("IDeckLinkProfileManager::GetProfile", hr);
                }
                bool active = false;
                if (profileObj->IsActive(&active) == S_OK && active)
                {
                    log::info("card_profile_already_active", {{"card", _displayName}, {"profile", config::profileName(profile)}});
                }
                else
                {
                    _selfProfileChange = true;
                    hr = profileObj->SetActive();
                    if (hr != S_OK)
                    {
                        _selfProfileChange = false;
                        return hresultMessage("IDeckLinkProfile::SetActive", hr);
                    }
                    log::info("card_profile_applied", {{"card", _displayName}, {"profile", config::profileName(profile)}});
                }
                return std::nullopt;
            }

            void setCallbacks(CardCallbacks callbacks) override
            {
                _callbacks = std::move(callbacks);
                // §3.9: register the profile callback on every sub-device.
                for (auto const& sub : _subDevices)
                {
                    auto mgr = sub->raw().queryInterface<IDeckLinkProfileManager>(IID_IDeckLinkProfileManager);
                    if (mgr)
                    {
                        mgr->SetCallback(this);
                        _profileManagers.push_back(std::move(mgr));
                    }
                }
            }

            // --- IDeckLinkProfileCallback ---------------------------------
            HRESULT ProfileChanging(IDeckLinkProfile* /*profileToBeActivated*/, bool streamsWillBeForcedToStop) override
            {
                log::warn("profile_changing", {{"card", _displayName}, {"streams_forced_to_stop", streamsWillBeForcedToStop}});
                return S_OK;
            }

            HRESULT ProfileActivated(IDeckLinkProfile* /*activatedProfile*/) override
            {
                if (_selfProfileChange.exchange(false))
                {
                    log::info("profile_activated_self", {{"card", _displayName}});
                    return S_OK;
                }
                // §3.9: external profile change → fail fast (exit code 2).
                log::error("profile_changed_externally", {{"card", _displayName}});
                if (_callbacks.onExternalProfileChange)
                {
                    _callbacks.onExternalProfileChange();
                }
                return S_OK;
            }

            // --- IUnknown --------------------------------------------------
            HRESULT QueryInterface(REFIID /*iid*/, LPVOID* ppv) override
            {
                *ppv = nullptr;
                return E_NOINTERFACE;
            }

            ULONG AddRef() override
            {
                return ++_refCount;
            }

            ULONG Release() override
            {
                return --_refCount;
            }

        private:
            std::string _displayName;
            std::uint32_t _persistentId;
            std::vector<std::unique_ptr<SdkSubDevice>> _subDevices;
            std::vector<ComPtr<IDeckLinkProfileManager>> _profileManagers;
            CardCallbacks _callbacks;
            std::atomic<bool> _selfProfileChange{false};
            std::atomic<ULONG> _refCount{1};
        };

        struct EnumeratedDevice
        {
            ComPtr<IDeckLink> device;
            SubDeviceInfo info;
            std::int64_t subDeviceIndexAttr = 0;
        };

        class SdkBackend final : public IBackend
        {
        public:
            std::variant<std::unique_ptr<ICard>, std::string> openCard(CardSelector const& selector) override
            {
                ComPtr<IDeckLinkIterator> iterator{::CreateDeckLinkIteratorInstance()};
                if (!iterator)
                {
                    return std::string("CreateDeckLinkIteratorInstance failed: libDeckLinkAPI.so not found or Desktop Video not installed");
                }

                std::vector<EnumeratedDevice> devices;
                ComPtr<IDeckLink> dev;
                while (iterator->Next(dev.receive()) == S_OK && dev)
                {
                    EnumeratedDevice e;
                    e.device = dev;

                    char const* name = nullptr;
                    if (dev->GetDisplayName(&name) == S_OK && name != nullptr)
                    {
                        e.info.displayName = name;
                    }
                    if (dev->GetModelName(&name) == S_OK && name != nullptr)
                    {
                        e.info.modelName = name;
                    }

                    auto attrs = dev.queryInterface<IDeckLinkProfileAttributes>(IID_IDeckLinkProfileAttributes);
                    if (attrs)
                    {
                        int64_t v = 0;
                        if (attrs->GetInt(BMDDeckLinkPersistentID, &v) == S_OK)
                        {
                            e.info.persistentId = static_cast<std::uint32_t>(v);
                        }
                        if (attrs->GetInt(BMDDeckLinkDeviceGroupID, &v) == S_OK)
                        {
                            e.info.deviceGroupId = v;
                        }
                        if (attrs->GetInt(BMDDeckLinkSubDeviceIndex, &v) == S_OK)
                        {
                            e.subDeviceIndexAttr = v;
                        }
                        bool flag = false;
                        if (attrs->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &flag) == S_OK)
                        {
                            e.info.supportsInputFormatDetection = flag;
                        }
                        if (attrs->GetInt(BMDDeckLinkVideoIOSupport, &v) == S_OK)
                        {
                            e.info.supportsCapture = (v & bmdDeviceSupportsCapture) != 0;
                            e.info.supportsPlayback = (v & bmdDeviceSupportsPlayback) != 0;
                        }
                    }
                    e.info.hasProfileManager = static_cast<bool>(dev.queryInterface<IDeckLinkProfileManager>(IID_IDeckLinkProfileManager));

                    log::info("decklink_enumerated",
                        {
                            {"display_name", e.info.displayName},
                            {"model", e.info.modelName},
                            {"persistent_id", e.info.persistentId},
                            {"group_id", e.info.deviceGroupId},
                            {"subdevice_index", e.subDeviceIndexAttr},
                        });
                    devices.push_back(std::move(e));
                }

                if (devices.empty())
                {
                    return std::string("no DeckLink devices found");
                }

                // Match per §3.1: persistent id preferred; name/index fallback.
                std::int64_t matchedGroup = 0;
                bool found = false;
                if (selector.persistentId)
                {
                    for (auto const& e : devices)
                    {
                        if (e.info.persistentId == *selector.persistentId ||
                            static_cast<std::uint32_t>(e.info.deviceGroupId) == *selector.persistentId)
                        {
                            matchedGroup = e.info.deviceGroupId;
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                    {
                        char buf[32];
                        ::snprintf(buf, sizeof(buf), "0x%08x", *selector.persistentId);
                        return std::string("no DeckLink card matches MXL_DECKLINK_CARD_ID ") + buf;
                    }
                }
                else if (selector.displayName)
                {
                    for (auto const& e : devices)
                    {
                        if (e.info.displayName.rfind(*selector.displayName, 0) == 0 || e.info.modelName == *selector.displayName)
                        {
                            matchedGroup = e.info.deviceGroupId;
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                    {
                        return "no DeckLink card matches MXL_DECKLINK_CARD_NAME '" + *selector.displayName + "'";
                    }
                }
                else
                {
                    // Card index over distinct group ids in enumeration order.
                    std::vector<std::int64_t> groups;
                    for (auto const& e : devices)
                    {
                        bool known = false;
                        for (auto const g : groups)
                        {
                            known = known || g == e.info.deviceGroupId;
                        }
                        if (!known)
                        {
                            groups.push_back(e.info.deviceGroupId);
                        }
                    }
                    int const idx = selector.index.value_or(0);
                    if (static_cast<std::size_t>(idx) >= groups.size())
                    {
                        return "MXL_DECKLINK_CARD_INDEX " + std::to_string(idx) + " out of range (" + std::to_string(groups.size()) + " cards)";
                    }
                    matchedGroup = groups[static_cast<std::size_t>(idx)];
                    found = true;
                }

                // Collect and order the card's sub-devices.
                std::vector<EnumeratedDevice> cardDevices;
                for (auto& e : devices)
                {
                    if (e.info.deviceGroupId == matchedGroup)
                    {
                        cardDevices.push_back(std::move(e));
                    }
                }
                std::sort(cardDevices.begin(), cardDevices.end(), [](EnumeratedDevice const& a, EnumeratedDevice const& b) {
                    return a.subDeviceIndexAttr < b.subDeviceIndexAttr;
                });

                std::vector<std::unique_ptr<SdkSubDevice>> subDevices;
                for (std::size_t i = 0; i < cardDevices.size(); ++i)
                {
                    auto& e = cardDevices[i];
                    e.info.subDeviceIndexOnCard = static_cast<int>(i);
                    subDevices.push_back(std::make_unique<SdkSubDevice>(std::move(e.device), std::move(e.info)));
                }

                auto const& first = subDevices.front()->info();
                log::info("decklink_card_matched",
                    {
                        {"display_name", first.displayName},
                        {"persistent_id", first.persistentId},
                        {"subdevices", static_cast<std::uint64_t>(subDevices.size())},
                    });

                return std::unique_ptr<ICard>(std::make_unique<SdkCard>(first.displayName, first.persistentId, std::move(subDevices)));
            }
        };
    }

    std::unique_ptr<IBackend> makeSdkBackend()
    {
        return std::make_unique<SdkBackend>();
    }
}
