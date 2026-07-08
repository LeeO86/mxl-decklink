// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "ops/metrics.hpp"

using namespace mxldl::ops;

TEST_CASE("counter and gauge rendering with labels")
{
    Registry reg;
    auto& c = reg.counter("mxl_decklink_frames_total", "Frames processed",
        {{"channel_index", "0"}, {"direction", "input"}, {"card_id", "0xa1b2c3d4"}});
    c.inc();
    c.inc(2);
    reg.gauge("mxl_decklink_channel_state", "state", {{"channel_index", "0"}}).set(1);

    auto const text = reg.render();
    CHECK(text.find("# TYPE mxl_decklink_frames_total counter") != std::string::npos);
    // Labels are sorted alphabetically.
    CHECK(text.find("mxl_decklink_frames_total{card_id=\"0xa1b2c3d4\",channel_index=\"0\",direction=\"input\"} 3") != std::string::npos);
    CHECK(text.find("mxl_decklink_channel_state{channel_index=\"0\"} 1") != std::string::npos);
}

TEST_CASE("same family with different label sets")
{
    Registry reg;
    reg.counter("frames", "f", {{"ch", "0"}}).inc();
    reg.counter("frames", "f", {{"ch", "1"}}).inc(5);
    auto const text = reg.render();
    CHECK(text.find("frames{ch=\"0\"} 1") != std::string::npos);
    CHECK(text.find("frames{ch=\"1\"} 5") != std::string::npos);
    // HELP/TYPE emitted once per family.
    CHECK(text.find("# TYPE frames counter") == text.rfind("# TYPE frames counter"));
}

TEST_CASE("histogram buckets are cumulative and include +Inf")
{
    Registry reg;
    auto& h = reg.histogram("latency_seconds", "l", {0.001, 0.01, 0.1}, {{"ch", "0"}});
    h.observe(0.0005);
    h.observe(0.005);
    h.observe(0.5); // above all bounds

    auto const text = reg.render();
    CHECK(text.find("latency_seconds_bucket{ch=\"0\",le=\"0.001\"} 1") != std::string::npos);
    CHECK(text.find("latency_seconds_bucket{ch=\"0\",le=\"0.01\"} 2") != std::string::npos);
    CHECK(text.find("latency_seconds_bucket{ch=\"0\",le=\"0.1\"} 2") != std::string::npos);
    CHECK(text.find("latency_seconds_bucket{ch=\"0\",le=\"+Inf\"} 3") != std::string::npos);
    CHECK(text.find("latency_seconds_count{ch=\"0\"} 3") != std::string::npos);
}

TEST_CASE("instrument identity: same name+labels returns the same instrument")
{
    Registry reg;
    auto& a = reg.counter("x", "x", {{"l", "1"}});
    auto& b = reg.counter("x", "x", {{"l", "1"}});
    CHECK(&a == &b);
    a.inc();
    b.inc();
    CHECK(a.value() == 2.0);
}
