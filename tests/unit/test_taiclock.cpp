// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "util/taiclock.hpp"

using namespace mxldl::util;

TEST_CASE("hardware clock calibration translates timestamps (§3.5)")
{
    HardwareClockCalibrator cal;
    CHECK_FALSE(cal.isCalibrated());
    CHECK(cal.needsRecalibration(0));

    // Hardware clock is TAI shifted by -1000 ns.
    std::uint64_t const tai = 1'000'000'000'000ULL;
    std::uint64_t const hw = tai - 1000;
    auto const applied = cal.calibrate(hw, tai);
    REQUIRE(applied.has_value());
    CHECK(*applied == 1000);
    CHECK(cal.isCalibrated());
    CHECK(cal.toTai(hw) == tai);
    CHECK(cal.toTai(hw + 500) == tai + 500);
}

TEST_CASE("recalibration below the 1 ms gate is applied")
{
    HardwareClockCalibrator cal;
    std::uint64_t const tai = 1'000'000'000'000ULL;
    cal.calibrate(tai, tai); // offset 0
    // 0.5 ms drift: applied.
    auto const delta = cal.calibrate(tai + 1'000'000, tai + 1'500'000);
    REQUIRE(delta.has_value());
    CHECK(*delta == 500'000);
    CHECK(cal.toTai(tai + 2'000'000) == tai + 2'500'000);
}

TEST_CASE("recalibration above the gate is rejected with the old offset kept (§3.5)")
{
    HardwareClockCalibrator cal;
    std::uint64_t const tai = 1'000'000'000'000ULL;
    cal.calibrate(tai, tai); // offset 0
    // 5 ms jump: rejected.
    auto const delta = cal.calibrate(tai, tai + 5'000'000);
    CHECK_FALSE(delta.has_value());
    CHECK(cal.toTai(tai) == tai); // old offset still in force
}

TEST_CASE("rolling recalibration interval (§3.5: 60 s)")
{
    HardwareClockCalibrator cal(60'000'000'000ULL, 1'000'000ULL);
    std::uint64_t const tai = 1'000'000'000'000ULL;
    cal.calibrate(tai, tai);
    CHECK_FALSE(cal.needsRecalibration(tai + 59'000'000'000ULL));
    CHECK(cal.needsRecalibration(tai + 61'000'000'000ULL));
}

TEST_CASE("taiNowNs advances monotonically")
{
    auto const a = taiNowNs();
    auto const b = taiNowNs();
    CHECK(b >= a);
    CHECK(a > 1'500'000'000ULL * 1'000'000'000ULL); // after ~2017 in TAI
}
