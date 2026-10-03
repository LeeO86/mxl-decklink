// SPDX-License-Identifier: MIT
// mxl-decklink entry point: startup sequence, signal handling and staged
// shutdown per SPECIFICATION.md §3.10.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mxl/mxl.h>

#include "channel/channel_manager.hpp"
#include "config/config.hpp"
#include "config/store.hpp"
#include "decklink/devicemanager.hpp"
#include "nmos/routing.hpp"
#include "mxlbridge/domain.hpp"
#include "mxlbridge/domainscan.hpp"
#include "ops/health.hpp"
#include "util/hostaddr.hpp"
#include "ops/housekeeping.hpp"
#include "ops/metrics.hpp"
#include "ops/webapi.hpp"
#include "util/logging.hpp"
#include "version.hpp"
#ifdef MXL_DECKLINK_NMOS
#include "nmos/node.hpp"
#endif

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

    bool tcpPortAccepts(int port)
    {
        int const fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
        {
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(port));
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        int const rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::close(fd);
        return rc == 0;
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

    // §3.10 step 1: configuration validation → EX_CONFIG on any violation.
    // §4.5: the store layers env over the MXL_CONFIG_FILE file layer.
    std::unique_ptr<mxldl::config::ConfigStore> store;
    mxldl::config::Config cfg;
    try
    {
        store = std::make_unique<mxldl::config::ConfigStore>(env);
        cfg = store->effectiveConfig();
    }
    catch (mxldl::config::ConfigError const& e)
    {
        mxldl::log::error("config_invalid", {{"details", e.what()}});
        return kExitConfig;
    }

        mxldl::log::configure(mxldl::log::parseLevel(cfg.logLevel), mxldl::log::parseFormat(cfg.logFormat));

        if (cfg.nmosEnable && cfg.nmosHostAddress.empty())
        {
            if (auto const ip = mxldl::util::firstNonLoopbackIpv4())
            {
                cfg.nmosHostAddress = *ip;
            }
            else
            {
                mxldl::log::error("config_invalid",
                    {{"details", "NMOS_HOST_ADDRESS is unset and this host has no non-loopback IPv4 address to announce"}});
                return kExitConfig;
            }
        }
        if (!cfg.outputDomainId.empty() || !cfg.nmosSeed.empty())
        {
            if (cfg.outputDomainId.empty() && !cfg.nmosSeed.empty())
            {
                cfg.outputDomainId = mxldl::nmosroute::idFromSeed(cfg.nmosSeed, "domain");
            }
        }

#ifndef MXL_DECKLINK_NMOS
    if (cfg.nmosEnable)
    {
        mxldl::log::error("nmos_not_built",
            {{"details", "NMOS_ENABLE=true but this binary was built without -DMXL_DECKLINK_NMOS=ON (Sony nmos-cpp)"}});
        return kExitConfig;
    }
#endif

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
        // §3.10 step 2: own output domain, then the MXL instance.
        try
        {
            cfg.outputDomainId = mxldl::mxlbridge::adoptOutputDomain(
                {cfg.domainPath, cfg.outputDomainId.empty() ? std::nullopt : std::optional<std::string>(cfg.outputDomainId), cfg.historyDurationNs});
        }
        catch (std::exception const& e)
        {
            auto const message = std::string(e.what());
            mxldl::log::error(message.rfind("domain_id_mismatch:", 0) == 0 ? "domain_id_mismatch" : "mxl_domain_failed", {{"details", message}});
            return message.rfind("domain_id_mismatch:", 0) == 0 ? kExitConfig : kExitTempFail;
        }
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
        bool cardDegraded = false;
        if (!card)
        {
            if (g_signalReceived.load() != 0)
            {
                return kExitForced;
            }
            // First-deploy / no-hardware path: keep the web UI reachable so
            // operators can finish configuration. Prefer the mock card when
            // no channels are active yet; otherwise fail as before.
            if (cfg.channels.empty() && cfg.webEnable)
            {
                mxldl::log::warn("card_open_fallback_mock",
                    {
                        {"details",
                            "DeckLink card open failed after retries; starting with the mock backend so the web UI stays reachable. "
                            "Set MXL_DECKLINK_CARD_ID (or fix the device) and restart before enabling channels in production."},
                    });
                auto mockEnv = env;
                auto mockCfg = cfg;
                mockCfg.backend = "mock";
                mockCfg.cardId.reset();
                mockCfg.cardName.reset();
                mockCfg.cardIndex = 0;
                auto result = mxldl::dl::openConfiguredCard(mockCfg, mockEnv);
                if (std::holds_alternative<std::unique_ptr<mxldl::dl::ICard>>(result))
                {
                    card = std::move(std::get<std::unique_ptr<mxldl::dl::ICard>>(result));
                    cardDegraded = true;
                    cfg.cardOpenFallback = true;
                    cfg.backend = "mock";
                }
            }
            if (!card)
            {
                mxldl::log::error("card_open_gave_up", {{"details", "startup retries exhausted (EX_TEMPFAIL)"}});
                return kExitTempFail;
            }
        }

        card->setCallbacks({.onExternalProfileChange = [] {
            g_externalProfileChange.store(true);
        }});

        if (cfg.cardProfile && !cardDegraded)
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
        bool const registrationRequired = cfg.nmosEnable && (!cfg.nmosRegistryAddress.empty() || cfg.nmosDnsSd);
#ifdef MXL_DECKLINK_NMOS
        std::unique_ptr<mxldl::nmosnode::Node> nmosNode;
#endif
        if (registrationRequired)
        {
            health.setReadyGate([&] {
#ifdef MXL_DECKLINK_NMOS
                return nmosNode && nmosNode->registered();
#else
                return false;
#endif
            });
        }

        mxldl::ops::Housekeeping housekeeping(cfg, channels, *domain, health, metrics);
        housekeeping.start();

#ifdef MXL_DECKLINK_NMOS
        if (cfg.nmosEnable)
        {
            try
            {
                nmosNode = std::make_unique<mxldl::nmosnode::Node>(cfg, channels, card->persistentId(), card->displayName());
                nmosNode->setPersist([&] {
                    std::map<std::string, std::optional<std::string>> changes;
                    for (auto const& ch : channels.channelConfigs())
                    {
                        auto const prefix = "CH" + std::to_string(ch.index) + "_";
                        auto consider = [&](std::string const& key, std::string const& value) {
                            if (store->sourceOf(key) == mxldl::config::SettingSource::Env)
                            {
                                return;
                            }
                            changes[key] = value;
                        };
                        consider(prefix + "MXL_VIDEO_FLOW_ID", ch.videoFlowId.toString());
                        consider(prefix + "MXL_ACTIVE", ch.videoMxlActive ? "true" : "false");
                        for (auto const& af : ch.audioFlows)
                        {
                            auto const afPrefix = prefix + "AF" + std::to_string(af.index) + "_";
                            consider(afPrefix + "FLOW_ID", af.flowId.toString());
                            consider(afPrefix + "MXL_ACTIVE", af.mxlActive ? "true" : "false");
                        }
                    }
                    if (changes.empty())
                    {
                        return;
                    }
                    auto const saved = store->update(changes);
                    if (std::holds_alternative<std::string>(saved))
                    {
                        mxldl::log::warn("nmos_activation_persist_failed", {{"details", std::get<std::string>(saved)}});
                    }
                });
                nmosNode->start();
            }
            catch (std::exception const& e)
            {
                mxldl::log::error("nmos_node_failed", {{"details", e.what()}});
                return kExitTempFail;
            }
        }
#endif
        channels.startAll();

        // §7.1/§7.5: the consolidated HTTP server (health + metrics always;
        // UI + API gated by WEB_ENABLE inside the service).
        mxldl::ops::WebService web(cfg, *store, channels, *card, *domain, health);
        try
        {
            web.start();
        }
        catch (std::exception const& e)
        {
            mxldl::log::error("http_server_failed", {{"details", e.what()}});
            return kExitTempFail;
        }

        if (!tcpPortAccepts(cfg.webPort) || (cfg.nmosEnable && (!tcpPortAccepts(cfg.nmosPort) || !tcpPortAccepts(cfg.nmosPort + 1))))
        {
            mxldl::log::error("port_bind_failed",
                {{"details", "a configured TCP port is not accepting connections"}, {"web_port", cfg.webPort}, {"nmos_port", cfg.nmosPort}});
            return kExitTempFail;
        }

        mxldl::log::info("running",
            {
                {"port", cfg.webPort},
                {"web_ui", cfg.webEnable},
            });

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
            exitCode = kExitForced;
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

        channels.stopAll(); // stop media and release MXL writers/readers
        mxldl::log::debug("shutdown_stage", {{"stage", "channels_stopped"}});
#ifdef MXL_DECKLINK_NMOS
        if (nmosNode)
        {
            nmosNode->stop(); // erase registry resources, then shut the node down
            nmosNode.reset();
        }
#endif
        web.stop();
        housekeeping.stop();
        mxldl::log::debug("shutdown_stage", {{"stage", "ops_stopped"}});
        auto const domainPath = domain->path();
        domain.reset(); // mxlDestroyInstance
        mxldl::log::debug("shutdown_stage", {{"stage", "mxl_destroyed"}});
        if (cfg.cleanupOnExit && exitCode == kExitForced)
        {
            auto const scan = cfg.domainScanPath;
            if (domainPath == scan || domainPath == "/" || std::filesystem::path(domainPath).filename().empty())
            {
                mxldl::log::error("mxl_cleanup_refused", {{"path", domainPath}, {"details", "refusing to remove the scan root or an empty path"}});
            }
            else
            {
                std::error_code ec;
                std::filesystem::remove_all(domainPath, ec);
                if (ec)
                {
                    mxldl::log::error("mxl_cleanup_failed", {{"path", domainPath}, {"details", ec.message()}});
                }
                else
                {
                    mxldl::log::info("mxl_domain_removed", {{"path", domainPath}});
                }
            }
        }
        card.reset();

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
