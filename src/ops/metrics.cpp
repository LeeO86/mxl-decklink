// SPDX-License-Identifier: MIT
#include "metrics.hpp"

#include <algorithm>
#include <cstdio>

#include "util/logging.hpp"

namespace mxldl::ops
{
    namespace
    {
        std::string formatDouble(double v)
        {
            if (v == static_cast<double>(static_cast<std::int64_t>(v)))
            {
                return std::to_string(static_cast<std::int64_t>(v));
            }
            char buf[64];
            ::snprintf(buf, sizeof(buf), "%g", v);
            return buf;
        }
    }

    Histogram::Histogram(std::vector<double> buckets)
        : _bounds(std::move(buckets))
        , _counts(_bounds.size() + 1, 0)
    {
        std::sort(_bounds.begin(), _bounds.end());
    }

    void Histogram::observe(double v)
    {
        std::lock_guard const lock{_mutex};
        std::size_t i = 0;
        while (i < _bounds.size() && v > _bounds[i])
        {
            ++i;
        }
        ++_counts[i];
        ++_count;
        _sum += v;
    }

    Histogram::Snapshot Histogram::snapshot() const
    {
        std::lock_guard const lock{_mutex};
        Snapshot s;
        s.bucketBounds = _bounds;
        s.bucketCounts.resize(_bounds.size());
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < _bounds.size(); ++i)
        {
            cumulative += _counts[i];
            s.bucketCounts[i] = cumulative;
        }
        s.count = _count;
        s.sum = _sum;
        return s;
    }

    std::string Registry::labelString(Labels const& labels)
    {
        if (labels.empty())
        {
            return "";
        }
        Labels sorted = labels;
        std::sort(sorted.begin(), sorted.end());
        std::string out = "{";
        bool first = true;
        for (auto const& [k, v] : sorted)
        {
            if (!first)
            {
                out += ',';
            }
            first = false;
            out += k;
            out += "=\"";
            out += log::jsonEscape(v);
            out += '"';
        }
        out += '}';
        return out;
    }

    Counter& Registry::counter(std::string const& name, std::string const& help, Labels const& labels)
    {
        std::lock_guard const lock{_mutex};
        auto& family = _families[name];
        family.help = help;
        family.type = "counter";
        auto& slot = family.counters[labelString(labels)];
        if (!slot)
        {
            slot = std::make_unique<Counter>();
        }
        return *slot;
    }

    Gauge& Registry::gauge(std::string const& name, std::string const& help, Labels const& labels)
    {
        std::lock_guard const lock{_mutex};
        auto& family = _families[name];
        family.help = help;
        family.type = "gauge";
        auto& slot = family.gauges[labelString(labels)];
        if (!slot)
        {
            slot = std::make_unique<Gauge>();
        }
        return *slot;
    }

    Histogram& Registry::histogram(std::string const& name, std::string const& help, std::vector<double> const& buckets, Labels const& labels)
    {
        std::lock_guard const lock{_mutex};
        auto& family = _families[name];
        family.help = help;
        family.type = "histogram";
        auto& slot = family.histograms[labelString(labels)];
        if (!slot)
        {
            slot = std::make_unique<Histogram>(buckets);
        }
        return *slot;
    }

    std::vector<double> Registry::latencyBuckets()
    {
        return {0.0001, 0.00025, 0.0005, 0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.1};
    }

    std::string Registry::render() const
    {
        std::lock_guard const lock{_mutex};
        std::string out;
        out.reserve(4096);
        for (auto const& [name, family] : _families)
        {
            out += "# HELP " + name + " " + family.help + "\n";
            out += "# TYPE " + name + " " + family.type + "\n";
            for (auto const& [labels, c] : family.counters)
            {
                out += name + labels + " " + formatDouble(c->value()) + "\n";
            }
            for (auto const& [labels, g] : family.gauges)
            {
                out += name + labels + " " + formatDouble(g->value()) + "\n";
            }
            for (auto const& [labels, h] : family.histograms)
            {
                auto const s = h->snapshot();
                // Insert the le label into the (possibly empty) label set.
                auto withLe = [&labels](std::string const& le) {
                    if (labels.empty())
                    {
                        return "{le=\"" + le + "\"}";
                    }
                    std::string mod = labels;
                    mod.pop_back();
                    mod += ",le=\"" + le + "\"}";
                    return mod;
                };
                for (std::size_t i = 0; i < s.bucketBounds.size(); ++i)
                {
                    out += name + "_bucket" + withLe(formatDouble(s.bucketBounds[i])) + " " + std::to_string(s.bucketCounts[i]) + "\n";
                }
                out += name + "_bucket" + withLe("+Inf") + " " + std::to_string(s.count) + "\n";
                out += name + "_sum" + labels + " " + formatDouble(s.sum) + "\n";
                out += name + "_count" + labels + " " + std::to_string(s.count) + "\n";
            }
        }
        return out;
    }
}
