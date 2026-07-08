// SPDX-License-Identifier: MIT
// Minimal Prometheus text-format registry (SPECIFICATION.md §7.3).
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mxldl::ops
{
    using Labels = std::vector<std::pair<std::string, std::string>>;

    class Counter
    {
    public:
        void inc(double v = 1.0)
        {
            double cur = _value.load(std::memory_order_relaxed);
            while (!_value.compare_exchange_weak(cur, cur + v, std::memory_order_relaxed))
            {
            }
        }

        [[nodiscard]] double value() const
        {
            return _value.load(std::memory_order_relaxed);
        }

    private:
        std::atomic<double> _value{0.0};
    };

    class Gauge
    {
    public:
        void set(double v)
        {
            _value.store(v, std::memory_order_relaxed);
        }

        [[nodiscard]] double value() const
        {
            return _value.load(std::memory_order_relaxed);
        }

    private:
        std::atomic<double> _value{0.0};
    };

    class Histogram
    {
    public:
        explicit Histogram(std::vector<double> buckets);

        void observe(double v);

        struct Snapshot
        {
            std::vector<double> bucketBounds;
            std::vector<std::uint64_t> bucketCounts; // cumulative
            std::uint64_t count = 0;
            double sum = 0.0;
        };

        [[nodiscard]] Snapshot snapshot() const;

    private:
        std::vector<double> _bounds;
        mutable std::mutex _mutex;
        std::vector<std::uint64_t> _counts;
        std::uint64_t _count = 0;
        double _sum = 0.0;
    };

    /// Registry keyed by (name, sorted labels). Thread-safe. Instruments are
    /// created once and cached by the channels; render() serializes all.
    class Registry
    {
    public:
        Counter& counter(std::string const& name, std::string const& help, Labels const& labels = {});
        Gauge& gauge(std::string const& name, std::string const& help, Labels const& labels = {});
        Histogram& histogram(std::string const& name, std::string const& help, std::vector<double> const& buckets, Labels const& labels = {});

        /// Prometheus text exposition format.
        [[nodiscard]] std::string render() const;

        /// Buckets per §7.3: 100 µs … 100 ms.
        static std::vector<double> latencyBuckets();

    private:
        struct Family
        {
            std::string help;
            std::string type;
            // label-string → instrument
            std::map<std::string, std::unique_ptr<Counter>> counters;
            std::map<std::string, std::unique_ptr<Gauge>> gauges;
            std::map<std::string, std::unique_ptr<Histogram>> histograms;
        };

        static std::string labelString(Labels const& labels);

        mutable std::mutex _mutex;
        std::map<std::string, Family> _families;
    };
}
