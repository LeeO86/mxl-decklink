// SPDX-License-Identifier: MIT
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

#include <doctest/doctest.h>
#include <mxl/time.h>

#include "util/audioconv.hpp"

using namespace mxldl::util;

TEST_CASE("int32 deinterleave to float32 (§2.3 scale)")
{
    // 3 frames × 2 channels, interleaved.
    std::vector<std::int32_t> const src = {
        1073741824, -1073741824, // frame 0: +0.5, -0.5
        2147483647, 0, // frame 1: ~+1.0, 0
        -2147483648, 214748364, // frame 2: -1.0, +0.1
    };
    // Layout: contiguous per-channel buffers, stride = 3 floats.
    mxlMutableWrappedMultiBufferSlice s{};
    s.count = 2;
    std::vector<float> storage(6, 0.0f);
    s.stride = 3 * sizeof(float);
    s.base.fragments[0].pointer = storage.data();
    s.base.fragments[0].size = 3 * sizeof(float);
    deinterleaveInt32ToFloat(src.data(), 3, 2, s);

    CHECK(storage[0] == doctest::Approx(0.5f));
    CHECK(storage[1] == doctest::Approx(1.0f));
    CHECK(storage[2] == doctest::Approx(-1.0f));
    CHECK(storage[3] == doctest::Approx(-0.5f));
    CHECK(storage[4] == doctest::Approx(0.0f));
    CHECK(storage[5] == doctest::Approx(0.1f));
}

TEST_CASE("deinterleave handles ring wraparound fragments")
{
    // 4 frames, 1 channel; ring of 4 with split 1+3.
    std::vector<std::int32_t> const src = {
        1 << 29, 2 << 29, -(1 << 29), -(2 << 29), // 0.25, 0.5, -0.25, -0.5
    };
    std::vector<float> ring(4, 0.0f);
    mxlMutableWrappedMultiBufferSlice s{};
    s.count = 1;
    s.stride = 4 * sizeof(float);
    s.base.fragments[0].pointer = ring.data() + 3; // last slot
    s.base.fragments[0].size = 1 * sizeof(float);
    s.base.fragments[1].pointer = ring.data(); // wrap to start
    s.base.fragments[1].size = 3 * sizeof(float);
    deinterleaveInt32ToFloat(src.data(), 4, 1, s);

    CHECK(ring[3] == doctest::Approx(0.25f)); // fragment 0
    CHECK(ring[0] == doctest::Approx(0.5f)); // fragment 1
    CHECK(ring[1] == doctest::Approx(-0.25f));
    CHECK(ring[2] == doctest::Approx(-0.5f));
}

TEST_CASE("int16 deinterleave scale")
{
    std::vector<std::int16_t> const src = {16384, -32768}; // 1 frame × 2 ch
    std::vector<float> storage(2, 0.0f);
    mxlMutableWrappedMultiBufferSlice s{};
    s.count = 2;
    s.stride = sizeof(float);
    s.base.fragments[0].pointer = storage.data();
    s.base.fragments[0].size = sizeof(float);
    deinterleaveInt16ToFloat(src.data(), 1, 2, s);
    CHECK(storage[0] == doctest::Approx(0.5f));
    CHECK(storage[1] == doctest::Approx(-1.0f));
}

TEST_CASE("float32 interleave round-trip (output path)")
{
    std::vector<float> storage = {0.5f, -0.5f, 1.5f, -1.5f}; // ch0: 0.5, -0.5; ch1: 1.5 (clamps), -1.5 (clamps)
    mxlWrappedMultiBufferSlice s{};
    s.count = 2;
    s.stride = 2 * sizeof(float);
    s.base.fragments[0].pointer = storage.data();
    s.base.fragments[0].size = 2 * sizeof(float);

    std::vector<std::int32_t> dst(4, 42);
    interleaveFloatToInt32(s, 2, 2, dst.data());
    CHECK(dst[0] == 1073741824); // 0.5 ch0 frame0
    CHECK(dst[1] == std::numeric_limits<std::int32_t>::max()); // 1.5 clamps to full scale
    CHECK(dst[2] == -1073741824);
    CHECK(dst[3] == std::numeric_limits<std::int32_t>::min());

    std::vector<std::int16_t> dst16(4, 42);
    interleaveFloatToInt16(s, 2, 2, dst16.data());
    CHECK(dst16[0] == 16384);
    CHECK(dst16[1] == std::numeric_limits<std::int16_t>::max());
    CHECK(dst16[2] == -16384);
    CHECK(dst16[3] == std::numeric_limits<std::int16_t>::min());
}

TEST_CASE("interleave fills missing channels with silence")
{
    std::vector<float> storage = {0.5f};
    mxlWrappedMultiBufferSlice s{};
    s.count = 1;
    s.stride = sizeof(float);
    s.base.fragments[0].pointer = storage.data();
    s.base.fragments[0].size = sizeof(float);

    std::vector<std::int32_t> dst(2, 42);
    interleaveFloatToInt32(s, 1, 2, dst.data());
    CHECK(dst[0] == 1073741824);
    CHECK(dst[1] == 0);
}

TEST_CASE("mapped deinterleave selects DeckLink channels by MAP")
{
    // 2 frames × 4 DeckLink channels; map flow ← {1, 3}
    std::vector<std::int32_t> const src = {
        0, 1073741824, 0, -1073741824, // frame 0
        0, 2147483647, 0, 0, // frame 1
    };
    std::vector<float> storage(4, 99.0f);
    mxlMutableWrappedMultiBufferSlice s{};
    s.count = 2;
    s.stride = 2 * sizeof(float);
    s.base.fragments[0].pointer = storage.data();
    s.base.fragments[0].size = 2 * sizeof(float);
    int const map[] = {1, 3};
    deinterleaveInt32ToFloatMapped(src.data(), 2, 4, map, s);
    CHECK(storage[0] == doctest::Approx(0.5f));
    CHECK(storage[1] == doctest::Approx(1.0f));
    CHECK(storage[2] == doctest::Approx(-0.5f));
    CHECK(storage[3] == doctest::Approx(0.0f));
}

TEST_CASE("mapped interleave writes into selected DeckLink channels")
{
    std::vector<float> storage = {0.5f, -0.5f}; // 1 frame × 2 flow ch
    mxlWrappedMultiBufferSlice s{};
    s.count = 2;
    s.stride = sizeof(float);
    s.base.fragments[0].pointer = storage.data();
    s.base.fragments[0].size = sizeof(float);
    std::vector<std::int32_t> dst(4, 7); // 1 frame × 4 DeckLink ch
    int const map[] = {1, 3};
    interleaveFloatToInt32Mapped(s, 1, map, 4, dst.data());
    CHECK(dst[0] == 7); // untouched
    CHECK(dst[1] == 1073741824);
    CHECK(dst[2] == 7);
    CHECK(dst[3] == -1073741824);
}

TEST_CASE("audio block of a frame on the TAI grid: whole rates unchanged, 29.97 / 59.94 cadence")
{
    // At today's TAI frame indexes (the math must hold there, not only near 0).
    // Whole samples per frame: exactly k x (48000 / rate), as before.
    for (auto const& [rate, perFrame] : {std::pair{mxlRational{25, 1}, 1920ULL}, std::pair{mxlRational{50, 1}, 960ULL},
             std::pair{mxlRational{24, 1}, 2000ULL}, std::pair{mxlRational{30, 1}, 1600ULL}, std::pair{mxlRational{60, 1}, 800ULL},
             std::pair{mxlRational{24000, 1001}, 2002ULL}})
    {
        CAPTURE(rate.numerator);
        auto const k0 = ::mxlGetCurrentIndex(&rate);
        for (std::uint64_t k = k0; k < k0 + 1000; ++k)
        {
            REQUIRE(audioSampleIndexOfFrame(rate, k) == k * perFrame);
        }
        CHECK(maxAudioSamplesPerFrame(rate) == perFrame);
    }

    // Not whole: the 5-frame cadence, 8008 resp. 4004 samples per 5 frames,
    // each block 1601/1602 resp. 800/801, and the index on the TAI grid.
    for (auto const& [rate, lo, five] : {std::tuple{mxlRational{30000, 1001}, 1601ULL, 8008ULL}, std::tuple{mxlRational{60000, 1001}, 800ULL, 4004ULL}})
    {
        CAPTURE(rate.numerator);
        mxlRational const audio{48000, 1};
        auto const k0 = ::mxlGetCurrentIndex(&rate);
        for (std::uint64_t k = k0; k < k0 + 1000; ++k)
        {
            auto const block = audioSampleIndexOfFrame(rate, k + 1) - audioSampleIndexOfFrame(rate, k);
            REQUIRE((block == lo || block == lo + 1));
            REQUIRE(audioSampleIndexOfFrame(rate, k + 5) - audioSampleIndexOfFrame(rate, k) == five);
            REQUIRE(audioSampleIndexOfFrame(rate, k) == ::mxlTimestampToIndex(&audio, ::mxlIndexToTimestamp(&rate, k)));
        }
        CHECK(maxAudioSamplesPerFrame(rate) == lo + 1);
        // Within a sample of the TAI time; frame index x floor(48000 /
        // rate) is days away from it.
        auto const tai = ::mxlIndexToTimestamp(&rate, k0);
        auto const exact = static_cast<double>(tai) * 48e-6;
        CHECK(std::abs(static_cast<double>(audioSampleIndexOfFrame(rate, k0)) - exact) <= 1.0);
        CHECK(std::abs(static_cast<double>(k0 * lo) - exact) > 48000.0 * 86400);
    }
}
