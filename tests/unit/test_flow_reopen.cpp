// SPDX-License-Identifier: MIT
// An output channel re-opens its MXL readers when the writer re-creates a
// flow under the same id (MXL_ERR_FLOW_INVALID), without a channel restart;
// reads video and audio from different domains; and rides out a source stall
// by repeating the last frame and playing silence, without a preroll reset.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <unistd.h>

#include <doctest/doctest.h>
#include <mxl/time.h>

#include "channel/output_channel.hpp"
#include "channel/reopen_schedule.hpp"
#include "config/videomodes.hpp"
#include "mxlbridge/audiowriter.hpp"
#include "mxlbridge/domain.hpp"
#include "mxlbridge/videowriter.hpp"
#include "ops/metrics.hpp"

using namespace mxldl;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

TEST_CASE("reopen schedule: at once, then 100 ms, 200 ms, 500 ms and every 1 s while the flow is missing")
{
    constexpr std::uint64_t ms = 1'000'000ULL;
    channel::ReopenSchedule s;
    CHECK_FALSE(s.pending());
    CHECK_FALSE(s.due(0));

    s.flowInvalid(1000 * ms);
    CHECK(s.pending());
    CHECK(s.due(1000 * ms));

    s.openFailed(1000 * ms);
    CHECK_FALSE(s.due(1099 * ms));
    CHECK(s.due(1100 * ms));
    s.openFailed(1100 * ms);
    CHECK(s.due(1300 * ms));
    s.openFailed(1300 * ms);
    CHECK_FALSE(s.due(1799 * ms));
    CHECK(s.due(1800 * ms));
    s.openFailed(1800 * ms);
    CHECK(s.due(2800 * ms));
    s.openFailed(2800 * ms);
    CHECK_FALSE(s.due(3799 * ms));
    CHECK(s.due(3800 * ms));
    CHECK(s.failures() == 5);

    // Degraded once the flow has been missing for 1 s.
    CHECK_FALSE(s.degraded(1999 * ms));
    CHECK(s.degraded(2000 * ms));

    // Another reader of the channel goes invalid meanwhile: due at once, the
    // event keeps its start and its failure count.
    s.flowInvalid(3000 * ms);
    CHECK(s.due(3000 * ms));
    CHECK(s.failures() == 5);
    CHECK(s.pendingForNs(3000 * ms) == 2000 * ms);

    s.clear();
    CHECK_FALSE(s.pending());
    CHECK_FALSE(s.due(10'000 * ms));
    CHECK_FALSE(s.degraded(10'000 * ms));
    CHECK(s.failures() == 0);
}

namespace
{
    constexpr std::uint8_t kGen1 = 0x11;
    // Video sources write the grain index at this byte offset of each frame;
    // stamped audio sources write the 20 ms block number (sample index / 960)
    // modulo kStampModulo, shifted left by 16, into every sample (exact
    // through MXL's float32 samples).
    constexpr std::size_t kStampAt = 8;
    constexpr std::uint64_t kStampModulo = 30000;
    constexpr std::uint8_t kGen2 = 0x22;

    /// What the fake card was asked to play.
    struct Played
    {
        std::mutex mutex;
        std::vector<std::uint8_t> frames; // first byte of every scheduled frame
        std::uint64_t audioBlocks = 0;
        std::uint64_t audioBlocksWithSound = 0;
        std::uint64_t late = 0; // completions reported DisplayedLate
        /// Stamped sources (kStampAt): the grain index of every scheduled
        /// frame, and per audio block the block number paired with the grain
        /// scheduled last (both modulo kStampModulo).
        std::vector<std::int64_t> grains;
        std::vector<std::pair<std::int64_t, std::int64_t>> avPairs;
        std::atomic<int> delayMs{0}; // the card holds the next completion this long

        /// Median of audio block - video grain over the pairs from `from`:
        /// constant while audio and video stay in sync.
        std::int64_t avOffset(std::size_t from)
        {
            std::lock_guard const lock{mutex};
            std::vector<std::int64_t> d;
            for (auto i = from; i < avPairs.size(); ++i)
            {
                d.push_back(avPairs[i].first - avPairs[i].second);
            }
            if (d.empty())
            {
                return 1'000'000;
            }
            std::sort(d.begin(), d.end());
            return d[d.size() / 2];
        }

        std::size_t pairCount()
        {
            std::lock_guard const lock{mutex};
            return avPairs.size();
        }

        /// Largest step between consecutive scheduled grains from `from` (1 =
        /// none skipped; a repeat is 0).
        std::int64_t largestGrainStep(std::size_t from)
        {
            std::lock_guard const lock{mutex};
            std::int64_t step = 0;
            for (auto i = std::max<std::size_t>(from, 1); i < grains.size(); ++i)
            {
                step = std::max(step, grains[i] - grains[i - 1]);
            }
            return step;
        }

        std::size_t grainCount()
        {
            std::lock_guard const lock{mutex};
            return grains.size();
        }

        std::uint64_t lateFrames()
        {
            std::lock_guard const lock{mutex};
            return late;
        }

        std::uint64_t blocks()
        {
            std::lock_guard const lock{mutex};
            return audioBlocks;
        }

        std::size_t frameCount()
        {
            std::lock_guard const lock{mutex};
            return frames.size();
        }

        std::size_t countSince(std::size_t from, std::uint8_t value)
        {
            std::lock_guard const lock{mutex};
            std::size_t n = 0;
            for (auto i = from; i < frames.size(); ++i)
            {
                n += frames[i] == value ? 1 : 0;
            }
            return n;
        }

        std::uint64_t soundBlocks()
        {
            std::lock_guard const lock{mutex};
            return audioBlocksWithSound;
        }
    };

    /// Completes one scheduled frame per frame period and pulls audio after
    /// each completion. Like a DeckLink output, frame n (ids from 1) is shown
    /// n - 1 periods after the start: a frame scheduled after that (the
    /// callbacks held it up) completes DisplayedLate.
    class FakePlayback final : public dl::IPlaybackSession
    {
    public:
        explicit FakePlayback(std::shared_ptr<Played> played)
            : _played(std::move(played))
        {}

        ~FakePlayback() override
        {
            stopPlayback();
        }

        std::optional<std::string> enable(Params const& params, dl::PlaybackCallbacks callbacks) override
        {
            _params = params;
            _callbacks = std::move(callbacks);
            return std::nullopt;
        }

        std::optional<std::uint64_t> scheduleFrame(std::uint8_t const* bytes, std::size_t size, std::int64_t, std::int64_t, std::int64_t) override
        {
            std::lock_guard const lock{_played->mutex};
            _played->frames.push_back(size > 0 ? bytes[0] : 0);
            if (size >= kStampAt + sizeof(std::uint64_t))
            {
                std::uint64_t index = 0;
                std::memcpy(&index, bytes + kStampAt, sizeof index);
                _played->grains.push_back(static_cast<std::int64_t>(index % kStampModulo));
            }
            auto const id = _nextId++;
            bool late = false;
            if (_running)
            {
                late = std::chrono::steady_clock::now() > _start + _period * static_cast<std::int64_t>(id - 1);
            }
            _queue.emplace_back(id, late);
            return id;
        }

        std::optional<std::string> scheduleAudio(void const* pcm, std::uint32_t sampleFrames) override
        {
            auto const n = static_cast<std::size_t>(sampleFrames) * static_cast<std::size_t>(_params.audioChannelCount);
            auto const* samples = static_cast<std::int32_t const*>(pcm);
            bool sound = false;
            for (std::size_t i = 0; i < n && !sound; ++i)
            {
                sound = samples[i] != 0;
            }
            std::lock_guard const lock{_played->mutex};
            ++_played->audioBlocks;
            _played->audioBlocksWithSound += sound ? 1 : 0;
            if (sound && !_played->grains.empty())
            {
                // A stamped source writes the block number into each sample.
                _played->avPairs.emplace_back(static_cast<std::int64_t>(samples[0] >> 16), _played->grains.back());
            }
            return std::nullopt;
        }

        std::optional<std::string> beginAudioPreroll() override
        {
            return std::nullopt;
        }

        std::optional<std::string> endAudioPreroll() override
        {
            return std::nullopt;
        }

        std::optional<std::string> startPlayback(std::int64_t, std::int64_t) override
        {
            auto const period = std::chrono::nanoseconds(1'000'000'000LL * _params.mode.rateDenominator / _params.mode.rateNumerator);
            {
                std::lock_guard const lock{_played->mutex};
                _period = period;
                _start = std::chrono::steady_clock::now();
                _running = true;
            }
            _ticker = std::thread([this, period] {
                auto next = _start + period;
                while (_running)
                {
                    std::this_thread::sleep_until(next);
                    next += period;
                    std::optional<std::uint64_t> id;
                    auto result = dl::CompletionResult::Completed;
                    {
                        std::lock_guard const lock{_played->mutex};
                        if (!_queue.empty())
                        {
                            id = _queue.front().first;
                            if (_queue.front().second)
                            {
                                result = dl::CompletionResult::DisplayedLate;
                                ++_played->late;
                            }
                            _queue.pop_front();
                        }
                    }
                    if (auto const delay = _played->delayMs.exchange(0); delay > 0)
                    {
                        // Output-side trouble: the card's calls come late,
                        // then in a burst (sleep_until is already past).
                        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                    }
                    if (id && _running)
                    {
                        _callbacks.onFrameCompleted(*id, result);
                        if (_params.audioEnable)
                        {
                            _callbacks.onRenderAudio(false);
                        }
                    }
                }
            });
            return std::nullopt;
        }

        void stopPlayback() override
        {
            _running = false;
            if (_ticker.joinable())
            {
                _ticker.join();
            }
        }

        void disable() override {}

        std::uint32_t bufferedVideoFrames() override
        {
            std::lock_guard const lock{_played->mutex};
            return static_cast<std::uint32_t>(_queue.size());
        }

        std::uint32_t bufferedAudioFrames() override
        {
            return 0;
        }

        bool referenceLocked() override
        {
            return true;
        }

    private:
        std::shared_ptr<Played> _played;
        Params _params;
        dl::PlaybackCallbacks _callbacks;
        std::deque<std::pair<std::uint64_t, bool>> _queue; // id, scheduled late
        std::chrono::steady_clock::time_point _start;
        std::chrono::nanoseconds _period{0};
        std::uint64_t _nextId = 1;
        std::atomic<bool> _running{false};
        std::thread _ticker;
    };

    class FakeSubDevice final : public dl::ISubDevice
    {
    public:
        explicit FakeSubDevice(std::shared_ptr<Played> played)
            : _played(std::move(played))
        {
            _info.displayName = "fake output";
            _info.supportsPlayback = true;
        }

        [[nodiscard]] dl::SubDeviceInfo const& info() const override
        {
            return _info;
        }

        [[nodiscard]] bool supportsMode(config::VideoMode const&, config::Direction) override
        {
            return true;
        }

        dl::SubDeviceStatus status() override
        {
            return {};
        }

        std::unique_ptr<dl::ICaptureSession> openCapture() override
        {
            return nullptr;
        }

        std::unique_ptr<dl::IPlaybackSession> openPlayback() override
        {
            return std::make_unique<FakePlayback>(_played);
        }

    private:
        std::shared_ptr<Played> _played;
        dl::SubDeviceInfo _info;
    };

    /// Runs `step` every few milliseconds on its own thread until destroyed.
    class Pump
    {
    public:
        explicit Pump(std::function<void()> step)
            : _thread([this, step = std::move(step)] {
                while (!_stop)
                {
                    step();
                    std::this_thread::sleep_for(2ms);
                }
            })
        {}

        ~Pump()
        {
            _stop = true;
            _thread.join();
        }

    private:
        std::atomic<bool> _stop{false};
        std::thread _thread;
    };

    /// A video writer that commits the grain of the current TAI time, every
    /// byte set to `fill`.
    class VideoSource
    {
    public:
        VideoSource(mxlbridge::Domain& domain, mxlbridge::VideoFlowParams const& p, std::uint8_t fill)
            : _writer(domain, p, 1)
            , _frame(_writer.grainPayloadSize() > 0 ? _writer.grainPayloadSize() : 1, fill)
            , _pump([this] {
                if (paused)
                {
                    return; // the flow stays, no new grains: a source stall
                }
                auto const rate = _writer.grainRate();
                // `offset` grains behind the current time; a larger offset
                // writes nothing until the time has caught up with it.
                auto const index = ::mxlGetCurrentIndex(&rate) - offset.load();
                if (index > _last)
                {
                    _last = index;
                    if (_frame.size() >= kStampAt + sizeof index)
                    {
                        std::memcpy(_frame.data() + kStampAt, &index, sizeof index);
                    }
                    (void)_writer.writeFrame(index, _frame.data(), _frame.size());
                }
            })
        {}

        std::atomic<bool> paused{false};
        std::atomic<std::uint64_t> offset{0}; // source latency in grains

    private:
        mxlbridge::VideoWriter _writer;
        std::vector<std::uint8_t> _frame;
        std::uint64_t _last = 0;
        Pump _pump; // last: stops before the writer goes
    };

    /// AudioSource value that stamps each sample with its block number.
    constexpr std::int32_t kStamped = -1;

    /// An audio writer that commits every sample up to the current TAI time
    /// less `lag` samples; with `block`, only whole blocks of that size.
    class AudioSource
    {
    public:
        AudioSource(mxlbridge::Domain& domain, mxlbridge::AudioFlowParams const& p, std::int32_t value, std::uint64_t block = 0, std::uint64_t lag = 0)
            : _block(block)
            , _lag(lag)
            , _writer(domain, p, 1)
            , _value(value)
            , _pump([this] {
                if (paused)
                {
                    _last = 0; // resumes at the current time, like a restarted origin
                    return;
                }
                auto const rate = _writer.sampleRate();
                auto const now = ::mxlGetCurrentIndex(&rate) - _lag;
                if (_last == 0)
                {
                    _last = now - 4800;
                }
                while (_last + std::max<std::uint64_t>(_block, 1) <= now)
                {
                    auto const count = _block != 0 ? _block : std::min<std::uint64_t>(now - _last, 480);
                    _pcm.assign(static_cast<std::size_t>(count) * 2, _value);
                    if (_value == kStamped)
                    {
                        for (std::uint64_t i = 0; i < count; ++i)
                        {
                            auto const stamp = static_cast<std::int32_t>(((_last + i) / 960 % kStampModulo) << 16);
                            _pcm[2 * i] = stamp;
                            _pcm[2 * i + 1] = stamp;
                        }
                    }
                    int const map[] = {0, 1};
                    (void)_writer.writeSamples(_last + count, _pcm.data(), static_cast<std::size_t>(count), 2, map, config::AudioSampleType::Int32);
                    _last += count;
                }
            })
        {}

        std::atomic<bool> paused{false};

    private:
        std::uint64_t _block;
        std::uint64_t _lag;
        mxlbridge::AudioWriter _writer;
        std::int32_t _value;
        std::uint64_t _last = 0;
        std::vector<std::int32_t> _pcm;
        Pump _pump;
    };

    struct Rig
    {
        fs::path path;
        std::unique_ptr<mxlbridge::Domain> domain;
        config::Config global;
        config::ChannelConfig ch;
        ops::Registry metrics;
        std::shared_ptr<Played> played = std::make_shared<Played>();
        std::unique_ptr<FakeSubDevice> sub = std::make_unique<FakeSubDevice>(played);
        mxlbridge::VideoFlowParams video;
        mxlbridge::AudioFlowParams audio;

        /// tmpfs when there is one (as in production): a domain on a disk
        /// can stall a read for writeback and make the timing checks flaky.
        static fs::path root()
        {
            std::error_code ec;
            return fs::is_directory("/dev/shm", ec) ? fs::path("/dev/shm") : fs::temp_directory_path();
        }

        explicit Rig(char const* name)
            : path(root() / (std::string("mxldl-reopen-") + name + "-" + std::to_string(::getpid())))
        {
            fs::remove_all(path);
            domain = std::make_unique<mxlbridge::Domain>(path.string());

            auto const mode = *config::lookupVideoMode("HD720p50");
            video.id = *util::parseUuid("5d1f0c55-3a0e-4f0b-8f4d-1c2a3b4c5d01");
            video.label = "reopen-video";
            video.groupHint = "reopen:Video";
            video.width = mode.width;
            video.height = mode.height;
            video.rateNumerator = mode.rateNumerator;
            video.rateDenominator = mode.rateDenominator;

            audio.id = *util::parseUuid("5d1f0c55-3a0e-4f0b-8f4d-1c2a3b4c5d02");
            audio.label = "reopen-audio";
            audio.groupHint = "reopen:Audio";
            audio.channelCount = 2;

            ch.index = 0;
            ch.label = "ch0";
            ch.direction = config::Direction::Output;
            ch.videoModeName = mode.name;
            ch.videoMode = mode;
            ch.videoFlowId = video.id;
            ch.audioEnable = false;
            ch.audioChannelCount = 2;
            ch.audioSampleType = config::AudioSampleType::Int32;
        }

        void withAudio()
        {
            ch.audioEnable = true;
            config::AudioFlowConfig af;
            af.index = 1;
            af.flowId = audio.id;
            af.channelCount = 2;
            af.deckLinkChannels = {0, 1};
            ch.audioFlows = {af};
        }

        ~Rig()
        {
            domain.reset();
            std::error_code ec;
            fs::remove_all(path, ec);
        }

        double reopens()
        {
            return metrics.counter("mxl_decklink_flow_reopens_total", "", labels()).value();
        }

        double reconnects()
        {
            return metrics.counter("mxl_decklink_reconnect_total", "", labels()).value();
        }

        double counter(char const* name, char const* label = nullptr, char const* value = nullptr)
        {
            auto l = labels();
            if (label != nullptr)
            {
                l.emplace_back(label, value);
            }
            return metrics.counter(name, "", l).value();
        }

        ops::Labels labels() const
        {
            return {
                {"card_id", "test"},
                {"channel_index", "0"},
                {"channel_label", "ch0"},
                {"direction", "output"},
                {"subdevice_index", "0"},
            };
        }
    };

    bool waitUntil(std::function<bool()> const& done, std::chrono::milliseconds limit)
    {
        auto const until = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < until)
        {
            if (done())
            {
                return true;
            }
            std::this_thread::sleep_for(10ms);
        }
        return done();
    }
}

TEST_CASE("output channel re-opens a re-created video flow and keeps playing")
{
    Rig rig("video");
    auto source = std::make_unique<VideoSource>(*rig.domain, rig.video, kGen1);
    std::this_thread::sleep_for(200ms);

    channel::OutputChannel out(rig.global, rig.ch, *rig.sub, *rig.domain, rig.metrics, "test");
    out.start();
    REQUIRE(waitUntil([&] { return out.status().state.load() == channel::State::Healthy; }, 3s));
    REQUIRE(waitUntil([&] { return rig.played->countSince(0, kGen1) >= 10; }, 2s));

    // The writer goes; the flow stays missing for longer than 1 s, then comes
    // back under the same id with a new data file.
    source.reset();
    auto const gapStart = rig.played->frameCount();
    std::this_thread::sleep_for(1500ms);
    // SDI never stops: the last frame is repeated while the flow is missing.
    CHECK(rig.played->frameCount() - gapStart >= 50);
    CHECK(out.status().state.load() == channel::State::Degraded);
    CHECK(rig.reopens() == 1.0);

    auto const before = rig.played->frameCount();
    source = std::make_unique<VideoSource>(*rig.domain, rig.video, kGen2);
    CHECK(waitUntil([&] { return rig.played->countSince(before, kGen2) >= 10; }, 3s));
    CHECK(waitUntil([&] { return out.status().state.load() == channel::State::Healthy; }, 1s));

    // One event, one count; no channel restart and no preroll reset.
    CHECK(rig.reopens() == 1.0);
    CHECK(rig.reconnects() == 0.0);
    CHECK(out.status().reconnects.load() == 0);

    out.stop();
    source.reset();
}

TEST_CASE("output channel re-opens a re-created audio flow of its sync group")
{
    Rig rig("audio");
    rig.withAudio();
    auto video = std::make_unique<VideoSource>(*rig.domain, rig.video, kGen1);
    auto audio = std::make_unique<AudioSource>(*rig.domain, rig.audio, 0x10000000);
    std::this_thread::sleep_for(200ms);

    channel::OutputChannel out(rig.global, rig.ch, *rig.sub, *rig.domain, rig.metrics, "test");
    out.start();
    REQUIRE(waitUntil([&] { return out.status().state.load() == channel::State::Healthy; }, 3s));
    REQUIRE(waitUntil([&] { return rig.played->soundBlocks() >= 10; }, 2s));

    // Re-create the audio flow at once (the usual writer restart).
    audio.reset();
    audio = std::make_unique<AudioSource>(*rig.domain, rig.audio, 0x10000000);
    CHECK(waitUntil([&] { return rig.reopens() == 1.0; }, 2s));
    auto const sound = rig.played->soundBlocks();
    auto const grains = out.status().grainsCommitted.load();
    CHECK(waitUntil([&] { return rig.played->soundBlocks() >= sound + 10; }, 2s));
    // Video went on from the live flow, not from repeated frames.
    CHECK(out.status().grainsCommitted.load() >= grains + 10);

    CHECK(rig.reopens() == 1.0);
    CHECK(rig.reconnects() == 0.0);
    CHECK(out.status().state.load() == channel::State::Healthy);

    out.stop();
    audio.reset();
    video.reset();
}

TEST_CASE("output channel reads video and audio from different domains and re-opens both")
{
    // Platform productions route the video and the audio of one output from
    // different functions, so from different MXL domains. 1.0.4 opened all
    // readers of an output on one instance of one domain: the audio flow was
    // not found (status 2) and every bring-up failed.
    Rig rig("mixed");
    rig.withAudio();
    auto const pathA = rig.path.string() + "-video-domain";
    auto const pathB = rig.path.string() + "-audio-domain";
    fs::remove_all(pathA);
    fs::remove_all(pathB);
    auto domainA = std::make_unique<mxlbridge::Domain>(pathA);
    auto domainB = std::make_unique<mxlbridge::Domain>(pathB);
    rig.ch.readerDomainPath = pathA;
    rig.ch.audioFlows[0].readerDomainPath = pathB;

    auto video = std::make_unique<VideoSource>(*domainA, rig.video, kGen1);
    auto audio = std::make_unique<AudioSource>(*domainB, rig.audio, 0x10000000);
    std::this_thread::sleep_for(200ms);

    {
        channel::OutputChannel out(rig.global, rig.ch, *rig.sub, *rig.domain, rig.metrics, "test");
        out.start();
        REQUIRE(waitUntil([&] { return out.status().state.load() == channel::State::Healthy; }, 3s));
        REQUIRE(waitUntil([&] { return rig.played->countSince(0, kGen1) >= 10 && rig.played->soundBlocks() >= 10; }, 2s));
        CHECK(rig.reconnects() == 0.0);

        // Both writers restart (their flows are re-created in their own domains).
        video.reset();
        audio.reset();
        auto const before = rig.played->frameCount();
        video = std::make_unique<VideoSource>(*domainA, rig.video, kGen2);
        audio = std::make_unique<AudioSource>(*domainB, rig.audio, 0x10000000);
        CHECK(waitUntil([&] { return rig.reopens() == 2.0; }, 3s));
        CHECK(waitUntil([&] { return rig.played->countSince(before, kGen2) >= 10; }, 3s));
        auto const sound = rig.played->soundBlocks();
        CHECK(waitUntil([&] { return rig.played->soundBlocks() >= sound + 10; }, 2s));
        CHECK(waitUntil([&] { return out.status().state.load() == channel::State::Healthy; }, 2s));
        CHECK(rig.reconnects() == 0.0);
        CHECK(out.status().reconnects.load() == 0);
        out.stop();
    }

    // The output never created a directory for a domain it only reads.
    auto const pathC = rig.path.string() + "-not-there";
    rig.ch.readerDomainPath = pathC;
    {
        channel::OutputChannel out(rig.global, rig.ch, *rig.sub, *rig.domain, rig.metrics, "test");
        out.start();
        std::this_thread::sleep_for(300ms);
        CHECK(out.status().state.load() != channel::State::Healthy);
        out.stop();
    }
    CHECK_FALSE(fs::exists(pathC));

    video.reset();
    audio.reset();
    domainA.reset();
    domainB.reset();
    std::error_code ec;
    fs::remove_all(pathA, ec);
    fs::remove_all(pathB, ec);
}

TEST_CASE("a source stall repeats the last frame and plays silence without a preroll reset")
{
    // A fabrics mirror whose origin restarts: the flow stays, no new grains
    // for a few seconds. 1.0.4 waited CHx_READER_TIMEOUT_MS in every DeckLink
    // callback (sync group, grain, audio), the card ran out of frames and
    // reported them late, and 25 late frames in 10 s reset the channel.
    Rig rig("stall");
    rig.withAudio();
    auto video = std::make_unique<VideoSource>(*rig.domain, rig.video, kGen1);
    auto audio = std::make_unique<AudioSource>(*rig.domain, rig.audio, 0x10000000);
    std::this_thread::sleep_for(200ms);

    channel::OutputChannel out(rig.global, rig.ch, *rig.sub, *rig.domain, rig.metrics, "test");
    out.start();
    REQUIRE(waitUntil([&] { return out.status().state.load() == channel::State::Healthy; }, 3s));
    REQUIRE(waitUntil([&] { return rig.played->soundBlocks() >= 10; }, 2s));

    video->paused = true;
    audio->paused = true;
    auto const frames = rig.played->frameCount();
    auto const blocks = rig.played->blocks();
    std::this_thread::sleep_for(1500ms);
    CHECK(out.status().state.load() == channel::State::Degraded);
    auto const soundAtStall = rig.played->soundBlocks();
    std::this_thread::sleep_for(1000ms);

    // 2.5 s at 50 Hz: about 125 frames and audio blocks, all on time.
    CHECK(rig.played->frameCount() - frames >= 100);
    CHECK(rig.played->blocks() - blocks >= 100);
    CHECK(rig.played->soundBlocks() == soundAtStall); // silence
    CHECK(rig.played->countSince(frames, kGen1) == rig.played->frameCount() - frames); // the last frame
    CHECK(rig.counter("mxl_decklink_frames_repeated_total", "reason", "source_stall") >= 100.0);
    CHECK(rig.counter("mxl_decklink_source_stalls_total", "essence", "video") == 1.0);
    CHECK(rig.counter("mxl_decklink_source_stalls_total", "essence", "audio") == 1.0);

    auto const grains = out.status().grainsCommitted.load();
    auto const sound = rig.played->soundBlocks();
    video->paused = false;
    audio->paused = false;
    CHECK(waitUntil([&] { return out.status().grainsCommitted.load() >= grains + 10; }, 2s));
    CHECK(waitUntil([&] { return rig.played->soundBlocks() >= sound + 10; }, 2s));
    CHECK(waitUntil([&] { return out.status().state.load() == channel::State::Healthy; }, 1s));

    // No output-side trouble: nothing late (a busy test host may delay a
    // frame or two) or dropped, no preroll reset, and the stall is not a
    // re-created flow.
    CHECK(rig.played->lateFrames() <= 2);
    CHECK(rig.counter("mxl_decklink_frames_late_total") == static_cast<double>(rig.played->lateFrames()));
    CHECK(rig.counter("mxl_decklink_frames_dropped_total") == 0.0);
    CHECK(out.status().framesDropped.load() == 0);
    CHECK(out.status().framesRepeated.load() >= 100);
    CHECK(rig.reconnects() == 0.0);
    CHECK(out.status().reconnects.load() == 0);
    CHECK(rig.reopens() == 0.0);

    out.stop();
    audio.reset();
    video.reset();
}

TEST_CASE("a source whose latency grows keeps playing: the read position slips behind it, audio in sync")
{
    // A mirror back from an origin restart with more buffering, or a card a
    // few ppm fast: the grains come ever later. A read position that stayed
    // ahead would repeat the last frame for good (the card is never late, so
    // no preroll reset re-aligns), with the channel healthy.
    Rig rig("late");
    rig.withAudio();
    auto video = std::make_unique<VideoSource>(*rig.domain, rig.video, kGen1);
    auto audio = std::make_unique<AudioSource>(*rig.domain, rig.audio, kStamped);
    std::this_thread::sleep_for(200ms);

    channel::OutputChannel out(rig.global, rig.ch, *rig.sub, *rig.domain, rig.metrics, "test");
    out.start();
    REQUIRE(waitUntil([&] { return out.status().state.load() == channel::State::Healthy; }, 3s));
    REQUIRE(waitUntil([&] { return rig.played->pairCount() >= 20; }, 2s));
    std::this_thread::sleep_for(300ms);
    auto const inSync = rig.played->avOffset(rig.played->pairCount() - 15);
    INFO("audio block - video grain at start: " << inSync);

    // One grain later every 300 ms, up to 4 grains (80 ms, beyond the 35 ms
    // the card lets a read wait).
    for (std::uint64_t offset = 1; offset <= 4; ++offset)
    {
        video->offset = offset;
        std::this_thread::sleep_for(300ms);
    }
    std::this_thread::sleep_for(300ms);
    auto const grains = out.status().grainsCommitted.load();
    auto const repeats = out.status().framesRepeated.load();
    auto const pairs = rig.played->pairCount();
    std::this_thread::sleep_for(1000ms);
    CHECK(out.status().grainsCommitted.load() - grains >= 40); // live frames, not a frozen one
    CHECK(out.status().framesRepeated.load() - repeats <= 2);
    CHECK(rig.counter("mxl_decklink_frames_repeated_total", "reason", "source_late") <= 16.0);
    CHECK(out.status().state.load() == channel::State::Healthy);
    // Each slip repeated an audio block too: audio is as far behind the
    // video grain as before.
    CHECK(rig.played->avOffset(pairs) == inSync);

    // The latency shrinks again: after a second behind, the read position
    // catches up with the source instead of lagging 4 grains behind it, and
    // audio skips as many blocks.
    video->offset = 0;
    std::this_thread::sleep_for(1500ms);
    out.housekeeping();
    CHECK(rig.metrics.gauge("mxl_decklink_reader_lag_grains", "", rig.labels()).value() <= 3.0);
    auto const after = out.status().grainsCommitted.load();
    auto const pairsAfter = rig.played->pairCount();
    std::this_thread::sleep_for(500ms);
    CHECK(out.status().grainsCommitted.load() - after >= 20);
    CHECK(rig.played->avOffset(pairsAfter) == inSync);

    CHECK(rig.played->lateFrames() <= 2);
    CHECK(rig.counter("mxl_decklink_frames_dropped_total") == 0.0);
    CHECK(rig.reconnects() == 0.0);
    out.stop();
    audio.reset();
    video.reset();
}

TEST_CASE("a burst of completions after an output-side delay skips no grain")
{
    // A loaded host holds the card's calls for 60 ms; they then come in a
    // burst and read behind the newest grain for a few frames. That is no
    // reason to skip ahead: every grain is still shown.
    Rig rig("burst");
    auto video = std::make_unique<VideoSource>(*rig.domain, rig.video, kGen1);
    std::this_thread::sleep_for(200ms);

    channel::OutputChannel out(rig.global, rig.ch, *rig.sub, *rig.domain, rig.metrics, "test");
    out.start();
    REQUIRE(waitUntil([&] { return out.status().state.load() == channel::State::Healthy; }, 3s));
    REQUIRE(waitUntil([&] { return rig.played->grainCount() >= 10; }, 2s));
    auto const from = rig.played->grainCount();
    for (int i = 0; i < 3; ++i)
    {
        rig.played->delayMs = 60;
        std::this_thread::sleep_for(400ms);
    }
    CHECK(rig.played->grainCount() - from >= 50);
    CHECK(rig.played->largestGrainStep(from) == 1);
    CHECK(out.status().framesRepeated.load() == 0);
    CHECK(rig.reconnects() == 0.0);
    out.stop();
    video.reset();
}

TEST_CASE("CHx_READ_OFFSET_GRAINS reads a lagging source behind its newest grain without repeats")
{
    // A mirrored source arrives 2 grains late (fabrics lag).
    Rig rig("offset");
    auto video = std::make_unique<VideoSource>(*rig.domain, rig.video, kGen1);
    video->offset = 2;
    std::this_thread::sleep_for(200ms);
    rig.ch.readOffsetGrains = 2;

    channel::OutputChannel out(rig.global, rig.ch, *rig.sub, *rig.domain, rig.metrics, "test");
    out.start();
    REQUIRE(waitUntil([&] { return out.status().state.load() == channel::State::Healthy; }, 3s));
    std::this_thread::sleep_for(1000ms);
    CHECK(out.status().grainsCommitted.load() >= 40);
    CHECK(out.status().framesRepeated.load() == 0);
    CHECK(rig.counter("mxl_decklink_source_stalls_total", "essence", "video") == 0.0);
    out.housekeeping();
    CHECK(rig.metrics.gauge("mxl_decklink_reader_lag_grains", "", rig.labels()).value() <= 3.0);
    out.stop();
    video.reset();
}

TEST_CASE("live audio written in whole frame blocks at the read position is not taken for stalled")
{
    // An audio writer that commits 20 ms blocks, the newest one right where
    // the output reads: its head does not move during one short wait, but it
    // moves every frame period. That is a live source, not a stall.
    Rig rig("blocks");
    rig.withAudio();
    auto video = std::make_unique<VideoSource>(*rig.domain, rig.video, kGen1);
    auto audio = std::make_unique<AudioSource>(*rig.domain, rig.audio, 0x10000000, 960, 2400);
    std::this_thread::sleep_for(200ms);

    channel::OutputChannel out(rig.global, rig.ch, *rig.sub, *rig.domain, rig.metrics, "test");
    out.start();
    REQUIRE(waitUntil([&] { return out.status().state.load() == channel::State::Healthy; }, 3s));
    REQUIRE(waitUntil([&] { return rig.played->soundBlocks() >= 10; }, 2s));
    auto const blocks = rig.played->blocks();
    auto const sound = rig.played->soundBlocks();
    std::this_thread::sleep_for(1500ms);
    CHECK(rig.played->blocks() - blocks >= 50);
    // Every block played has sound: none became silence.
    CHECK(rig.played->blocks() - blocks == rig.played->soundBlocks() - sound);
    CHECK(rig.counter("mxl_decklink_source_stalls_total", "essence", "audio") == 0.0);
    CHECK(out.status().state.load() == channel::State::Healthy);
    out.stop();
    audio.reset();
    video.reset();
}

TEST_CASE("CHx_READ_OFFSET_GRAINS beyond the flows' history is capped")
{
    // 64 grains behind is far outside a 200 ms history (10 grains at 50p):
    // uncapped, the preroll would read grains that are gone. The output reads
    // as far back as the video and audio rings allow and plays.
    Rig rig("offsetcap");
    rig.withAudio();
    auto video = std::make_unique<VideoSource>(*rig.domain, rig.video, kGen1);
    auto audio = std::make_unique<AudioSource>(*rig.domain, rig.audio, kStamped);
    std::this_thread::sleep_for(300ms);
    rig.ch.readOffsetGrains = 64;

    channel::OutputChannel out(rig.global, rig.ch, *rig.sub, *rig.domain, rig.metrics, "test");
    out.start();
    REQUIRE(waitUntil([&] { return out.status().state.load() == channel::State::Healthy; }, 3s));
    auto const grains = out.status().grainsCommitted.load();
    auto const sound = rig.played->soundBlocks();
    std::this_thread::sleep_for(1000ms);
    CHECK(out.status().grainsCommitted.load() - grains >= 40);
    CHECK(rig.played->soundBlocks() - sound >= 40);
    CHECK(out.status().framesRepeated.load() <= 2);
    CHECK(rig.reconnects() == 0.0);
    out.housekeeping();
    auto const lag = rig.metrics.gauge("mxl_decklink_reader_lag_grains", "", rig.labels()).value();
    CHECK(lag >= 2.0); // still behind on purpose, as far as the rings allow
    CHECK(lag <= 9.0);
    out.stop();
    audio.reset();
    video.reset();
}
