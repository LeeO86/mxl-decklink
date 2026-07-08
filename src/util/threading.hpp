// SPDX-License-Identifier: MIT
// Realtime scheduling and CPU pinning per SPECIFICATION.md §2.5.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mxldl::util
{
    /// Parses a comma-separated CPU list ("0,1,4-7"); returns nullopt on
    /// malformed input.
    std::optional<std::vector<int>> parseCpuList(std::string_view s);

    /// Applies SCHED_FIFO at `priority` to the calling thread. Returns an
    /// error string on failure (typically missing CAP_SYS_NICE).
    std::optional<std::string> setRealtimePriority(int priority);

    /// Pins the calling thread to the given CPUs.
    std::optional<std::string> pinToCpus(std::vector<int> const& cpus);

    /// Names the calling thread for debugging (best effort).
    void setThreadName(std::string_view name);
}
