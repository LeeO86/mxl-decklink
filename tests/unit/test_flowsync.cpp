// SPDX-License-Identifier: MIT
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

#include <unistd.h>

#include <doctest/doctest.h>

#include "mxlbridge/audioreader.hpp"
#include "mxlbridge/audiowriter.hpp"
#include "mxlbridge/domain.hpp"
#include "mxlbridge/flowsync.hpp"

using namespace mxldl;
using namespace mxldl::mxlbridge;
namespace fs = std::filesystem;

TEST_CASE("sync groups come and go while another channel releases readers of the same domain")
{
    // One output channel creates and releases its sync group (bring-up,
    // preroll reset, stop) while another channel releases its readers of the
    // same domain. With the group on the shared domain instance, MXL walked a
    // group being released: SIGSEGV in FlowSynchronizationGroup::removeReader.
    auto const path = fs::temp_directory_path() / ("mxldl-sync-" + std::to_string(::getpid()));
    {
        Domain domain(path.string());
        AudioFlowParams p;
        p.id = *util::parseUuid("0b5c3a3e-6f3c-4c1e-9d1a-5a4e2f1b7c11");
        p.label = "sync-race";
        p.groupHint = "sync-race:Audio";
        p.channelCount = 2;
        AudioWriter writer(domain, p, 1);

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> released{0};
        std::thread other([&] {
            while (!stop)
            {
                AudioReader reader(domain, writer.flowId());
                ++released;
            }
        });
        std::uint64_t groups = 0;
        auto const until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < until)
        {
            FlowSyncGroup sync(domain.path());
            ++groups;
        }
        stop = true;
        other.join();
        CHECK(groups > 0);
        CHECK(released > 0);
    }
    std::error_code ec;
    fs::remove_all(path, ec);
}
