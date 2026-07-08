// SPDX-License-Identifier: MIT
// mxl-decklink entry point: startup sequence, signal handling and staged
// shutdown per SPECIFICATION.md §3.10.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <thread>

#include <mxl/mxl.h>

#include "channel/channel_manager.hpp"
#include "config/config.hpp"
#include "decklink/devicemanager.hpp"
#include "mxlbridge/domain.hpp"
#include "ops/health.hpp"
#include "ops/housekeeping.hpp"
#include "ops/metrics.hpp"
#include "util/logging.hpp"
#include "version.hpp"

namespace
{
    // sysexits.h codes required by §3.10, plus §3.9's profile exit.
    constexpr int kExitOk = 0;
    constexpr int kExitProfileChanged = 2;
    constexpr int kExitTempFail = 75; // EX_TEMPFAIL
    constexpr int kExitConfig = 78; // EX_CONFIG
    constexpr int kExitForced = 143; // 128 + SIGTERM

    std::atomic<int> g_signalReceived{0};
    std::atomic<bool> g_externalProfileChange{false};

    void signalHandler(int sig)
    {
        g_signalReceived.store(sig);
    }

    /// Card-level startup with exponential backoff (§3.10: initial 1 s, max
    /// 30 s, up to STARTUP_MAX_RETRIES, then EX_TEMPFAIL).
    std::unique_ptr<mxldl::dl::ICard> openCardWithRetry(mxldl::config::Config const& cfg, mxldl::config::EnvReader const& env)
    {
        std::uint64_t backoffMs = 1000;
        for (int attempt = 0; attempt <= cfg.startupMaxRetries; ++attempt)
        {
            if (g_signalReceived.load() != 0)
            {
                return nullptr;
            }
            auto result = mxldl::dl::openConfiguredCard(cfg, env);
            if (std::holds_alternative<std::unique_ptr<mxldl::dl::ICard>>(result))
            {
                return std::move(std::get<std::unique_ptr<mxldl::dl::ICard>>(result));
            }
            mxldl::log::error("card_open_failed",
                {
                    {"attempt", attempt + 1},
                    {"max_retries", cfg.startupMaxRetries},
                    {"details", std::get<std::string>(result)},
                });
            if (attempt < cfg.startupMaxRetries)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
                backoffMs = backoffMs * 2 > 30000 ? 30000 : backoffMs * 2;
            }
        }
        return nullptr;
    }
}

int main()
{
    auto const env = mxldl::config::systemEnv();

    // §3.10 step 1: ENV validation → EX_CONFIG on any violation.
    mxldl::config::Config cfg;
    try
    {
        cfg = mxldl::config::loadConfig(env);
    }
    catch (mxldl::config::ConfigError const& e)
    {
        mxldl::log::error("config_invalid", {{"details", e.what()}});
        return kExitConfig;
    }

    mxldl::log::configure(mxldl::log::parseLevel(cfg.logLevel), mxldl::log::parseFormat(cfg.logFormat));

    mxlVersionType mxlVersion{};
    ::mxlGetVersion(&mxlVersion);
    mxldl::log::info("starting",
        {
            {"version", mxldl::kVersion},
            {"mxl_version", mxlVersion.full != nullptr ? mxlVersion.full : "?"},
            {"channels", static_cast<std::uint64_t>(cfg.channels.size())},
            {"legacy_mode", cfg.legacyMode},
            {"backend", cfg.backend},
        });

    ::signal(SIGTERM, signalHandler);
    ::signal(SIGINT, signalHandler);
    ::signal(SIGPIPE, SIG_IGN);

    int exitCode = kExitOk;
    try
    {
        // §3.10 step 2: MXL instance.
        std::unique_ptr<mxldl::mxlbridge::Domain> domain;
        try
        {
            domain = std::make_unique<mxldl::mxlbridge::Domain>(cfg.domainPath);
        }
        catch (std::exception const& e)
        {
            mxldl::log::error("mxl_domain_failed", {{"details", e.what()}});
            return kExitTempFail;
        }

        // §3.10 steps 3–4: enumeration, card match, profile.
        auto card = openCardWithRetry(cfg, env);
        if (!card)
        {
            if (g_signalReceived.load() != 0)
            {
                return kExitOk;
            }
            mxldl::log::error("card_open_gave_up", {{"details", "startup retries exhausted (EX_TEMPFAIL)"}});
            return kExitTempFail;
        }

        card->setCallbacks({.onExternalProfileChange = [] {
            g_externalProfileChange.store(true);
        }});

        if (cfg.cardProfile)
        {
            if (auto const err = card->applyProfile(*cfg.cardProfile))
            {
                mxldl::log::error("card_profile_failed", {{"details", *err}});
                return kExitTempFail;
            }
        }

        // §3.10 steps 5–9 run per channel inside the channel supervisors
        // (channel-level failures never end the process, §3.7).
        mxldl::ops::Registry metrics;
        mxldl::channel::ChannelManager channels(cfg, *card, *domain, metrics);

        mxldl::ops::HealthService health(cfg, channels, metrics);
        try
        {
            health.start();
        }
        catch (std::exception const& e)
        {
            mxldl::log::error("http_server_failed", {{"details", e.what()}});
            return kExitTempFail;
        }

        mxldl::ops::Housekeeping housekeeping(cfg, channels, *domain, health, metrics);
        housekeeping.start();
        channels.startAll();

        mxldl::log::info("running", {{"health_port", cfg.healthPort}, {"metrics_port", cfg.metricsPort}});

        // Main wait loop.
        while (g_signalReceived.load() == 0 && !g_externalProfileChange.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        if (g_externalProfileChange.load())
        {
            // §3.9: fail fast; the orchestrator restarts the pod.
            mxldl::log::error("exiting_profile_changed_externally", {{"exit_code", kExitProfileChanged}});
            exitCode = kExitProfileChanged;
        }
        else
        {
            mxldl::log::info("shutdown_signal", {{"signal", g_signalReceived.load()}});
        }

        // §3.10 staged shutdown bounded by SHUTDOWN_TIMEOUT_S; a watchdog
        // forces exit 143 when the stages overrun.
        std::atomic<bool> shutdownDone{false};
        std::thread watchdog([&] {
            auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(cfg.shutdownTimeoutS);
            while (!shutdownDone.load())
            {
                if (std::chrono::steady_clock::now() > deadline)
                {
                    mxldl::log::error("shutdown_timeout_forced_exit", {{"exit_code", kExitForced}});
                    std::_Exit(kExitForced);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });

        channels.stopAll(); // steps 1–4: stop streams, flush, disable, release writers/readers
        housekeeping.stop();
        health.stop();
        domain.reset(); // step 5: mxlDestroyInstance
        card.reset(); // step 6: release DeckLink refs

        shutdownDone.store(true);
        watchdog.join();
        mxldl::log::info("shutdown_complete", {{"exit_code", exitCode}});
    }
    catch (std::exception const& e)
    {
        mxldl::log::error("fatal", {{"details", e.what()}});
        return kExitTempFail;
    }

    return exitCode;
}
