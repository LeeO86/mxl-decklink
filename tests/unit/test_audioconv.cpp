// SPDX-License-Identifier: MIT
#include <cmath>
#include <limits>
#include <vector>

#include <doctest/doctest.h>

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
