// SPDX-License-Identifier: MIT
#include "taiclock.hpp"

#include <ctime>

#include "logging.hpp"

namespace mxldl::util
{
    std::uint64_t taiNowNs()
    {
        timespec ts{};
        ::clock_gettime(CLOCK_TAI, &ts);
        return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(ts.tv_nsec);
    }

    HardwareClockCalibrator::HardwareClockCalibrator(std::uint64_t recalibrationIntervalNs, std::uint64_t maxStepNs)
        : _recalibrationIntervalNs(recalibrationIntervalNs)
        , _maxStepNs(maxStepNs)
    {}

    std::optional<std::int64_t> HardwareClockCalibrator::calibrate(std::uint64_t hardwareNowNs, std::uint64_t taiNow)
    {
        std::lock_guard const lock{_mutex};
        auto const newOffset = static_cast<std::int64_t>(taiNow) - static_cast<std::int64_t>(hardwareNowNs);
        if (_calibrated)
        {
            auto const delta = newOffset - _offsetNs;
            auto const magnitude = static_cast<std::uint64_t>(delta < 0 ? -delta : delta);
            if (magnitude > _maxStepNs)
            {
                // §3.5: deltas above the gate indicate drift/discontinuity on
                // non-genlocked systems; warn instead of stepping timestamps.
                log::warn("hw_clock_recalibration_rejected",
                    {
                        {"delta_ns", delta},
                        {"max_step_ns", _maxStepNs},
                    });
                _lastCalibrationTai = taiNow;
                return std::nullopt;
            }
            _offsetNs = newOffset;
            _lastCalibrationTai = taiNow;
            return delta;
        }
        _offsetNs = newOffset;
        _calibrated = true;
        _lastCalibrationTai = taiNow;
        return newOffset;
    }

    bool HardwareClockCalibrator::isCalibrated() const
    {
        std::lock_guard const lock{_mutex};
        return _calibrated;
    }

    bool HardwareClockCalibrator::needsRecalibration(std::uint64_t taiNow) const
    {
        std::lock_guard const lock{_mutex};
        if (!_calibrated)
        {
            return true;
        }
        return taiNow - _lastCalibrationTai >= _recalibrationIntervalNs;
    }

    std::uint64_t HardwareClockCalibrator::toTai(std::uint64_t hardwareTimestampNs) const
    {
        std::lock_guard const lock{_mutex};
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(hardwareTimestampNs) + _offsetNs);
    }
}
