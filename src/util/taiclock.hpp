// SPDX-License-Identifier: MIT
// TAI timestamping per SPECIFICATION.md §3.5.
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

namespace mxldl::util
{
    /// Nanoseconds since the SMPTE ST 2059-1 / PTP epoch (1970-01-01 TAI),
    /// read from CLOCK_TAI. Matches MXL's own mxlGetTime().
    std::uint64_t taiNowNs();

    /// Timestamp source selection (§3.5): `host` uses CLOCK_TAI at callback
    /// entry, `hardware` offset-calibrates the card's reference clock against
    /// CLOCK_TAI and recalibrates on a rolling basis.
    enum class TimestampSource
    {
        Hardware,
        Host,
    };

    /// Calibrates a monotonic hardware reference timeline against CLOCK_TAI.
    /// Thread-safe; used from streaming threads (translate) and the
    /// housekeeping thread (recalibrate).
    class HardwareClockCalibrator
    {
    public:
        /// \param recalibrationIntervalNs rolling recalibration period (§3.5: 60 s)
        /// \param maxStepNs delta magnitude above which recalibration is
        ///        refused with a warning instead of applied (§3.5: 1 ms)
        explicit HardwareClockCalibrator(std::uint64_t recalibrationIntervalNs = 60'000'000'000ULL, std::uint64_t maxStepNs = 1'000'000ULL);

        /// Establish (or re-establish) the offset from a paired observation of
        /// the hardware clock and CLOCK_TAI. Returns the applied delta in ns,
        /// or nullopt if the delta exceeded the sanity gate and only a warning
        /// was recorded.
        std::optional<std::int64_t> calibrate(std::uint64_t hardwareNowNs, std::uint64_t taiNowNs);

        /// True if `calibrate` has succeeded at least once.
        [[nodiscard]] bool isCalibrated() const;

        /// True when the rolling recalibration interval has elapsed.
        [[nodiscard]] bool needsRecalibration(std::uint64_t taiNowNs) const;

        /// Translate a hardware timestamp to TAI ns. Requires isCalibrated().
        [[nodiscard]] std::uint64_t toTai(std::uint64_t hardwareTimestampNs) const;

    private:
        std::uint64_t const _recalibrationIntervalNs;
        std::uint64_t const _maxStepNs;
        mutable std::mutex _mutex;
        bool _calibrated = false;
        std::int64_t _offsetNs = 0; // tai = hw + offset
        std::uint64_t _lastCalibrationTai = 0;
    };
}
