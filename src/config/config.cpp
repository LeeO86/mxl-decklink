// SPDX-License-Identifier: MIT
#include "config.hpp"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <unordered_map>

#include "util/threading.hpp"

namespace mxldl::config
{
    namespace
    {
        constexpr int kMaxChannels = 16; // §4: CHx_, x = 0..15

        [[noreturn]] void fail(std::string const& msg)
        {
            throw ConfigError(msg);
        }

        int parseInt(std::string const& name, std::string const& value, long min, long max)
        {
            try
            {
                std::size_t consumed = 0;
                long const v = std::stol(value, &consumed, 10);
                if (consumed != value.size())
                {
                    fail(name + ": not an integer: '" + value + "'");
                }
                if (v < min || v > max)
                {
                    fail(name + ": value " + std::to_string(v) + " out of range [" + std::to_string(min) + ", " + std::to_string(max) + "]");
                }
                return static_cast<int>(v);
            }
            catch (ConfigError const&)
            {
                throw;
            }
            catch (...)
            {
                fail(name + ": not an integer: '" + value + "'");
            }
        }

        bool parseBool(std::string const& name, std::string const& value)
        {
            if (value == "true" || value == "1" || value == "yes" || value == "on")
            {
                return true;
            }
            if (value == "false" || value == "0" || value == "no" || value == "off")
            {
                return false;
            }
            fail(name + ": not a boolean: '" + value + "'");
        }

        util::Uuid parseUuidOrFail(std::string const& name, std::string const& value)
        {
            auto const parsed = util::parseUuid(value);
            if (!parsed)
            {
                fail(name + ": not a valid UUID: '" + value + "'");
            }
            return *parsed;
        }

        std::uint32_t parseHex32(std::string const& name, std::string const& value)
        {
            std::string v = value;
            if (v.rfind("0x", 0) == 0 || v.rfind("0X", 0) == 0)
            {
                v = v.substr(2);
            }
            if (v.empty() || v.size() > 8 || v.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            {
                fail(name + ": not a 32-bit hex value: '" + value + "'");
            }
            return static_cast<std::uint32_t>(std::stoul(v, nullptr, 16));
        }

        /// Wraps EnvReader with a per-prefix getter and presence tracking.
        struct Env
        {
            EnvReader const& reader;

            std::optional<std::string> get(std::string const& name) const
            {
                auto v = reader(name);
                if (v && v->empty())
                {
                    // Empty values are treated as unset: this is the natural
                    // Kubernetes ConfigMap behavior for optional entries.
                    return std::nullopt;
                }
                return v;
            }

            bool has(std::string const& name) const
            {
                return get(name).has_value();
            }
        };

        // §4.4: the legacy v1.0 single-channel variable set.
        std::vector<std::string> const kLegacyVars = {
            "DIRECTION",
            "DECKLINK_DEVICE_ID",
            "DECKLINK_DEVICE_NAME",
            "DECKLINK_DEVICE_INDEX",
            "VIDEO_MODE",
            "PIXEL_FORMAT",
            "ALLOW_FORMAT_CONVERSION",
            "VIDEO_ANC_ENABLE",
            "AUDIO_ENABLE",
            "AUDIO_CHANNEL_COUNT",
            "AUDIO_SAMPLE_TYPE",
            "MXL_VIDEO_FLOW_ID",
            "MXL_AUDIO_FLOW_ID",
            "MXL_ANC_FLOW_ID",
            "MXL_VIDEO_FLOW_LABEL",
            "MXL_AUDIO_FLOW_LABEL",
            "MXL_GROUP_HINT",
            "MXL_DEVICE_ID",
            "MXL_SOURCE_ID",
            "GRAIN_COUNT",
            "AUDIO_BUFFER_MS",
            "COMMIT_BATCH_HINT",
            "OUTPUT_PREROLL_GRAINS",
            "READER_TIMEOUT_MS",
            "CHANNEL_LABEL",
        };

        std::vector<std::string> presentLegacyVars(Env const& env)
        {
            std::vector<std::string> present;
            for (auto const& v : kLegacyVars)
            {
                if (env.has(v))
                {
                    present.push_back(v);
                }
            }
            return present;
        }

        std::vector<int> presentChannelIndices(Env const& env)
        {
            std::vector<int> indices;
            for (int i = 0; i < kMaxChannels; ++i)
            {
                if (env.has("CH" + std::to_string(i) + "_DIRECTION"))
                {
                    indices.push_back(i);
                }
            }
            return indices;
        }

        ChannelConfig parseChannel(Env const& env, int index, std::string const& prefix)
        {
            auto key = [&](char const* suffix) {
                return prefix + suffix;
            };

            ChannelConfig ch;
            ch.index = index;

            auto const dir = env.get(key("DIRECTION"));
            if (!dir)
            {
                fail(key("DIRECTION") + " is required");
            }
            if (*dir == "input")
            {
                ch.direction = Direction::Input;
            }
            else if (*dir == "output")
            {
                ch.direction = Direction::Output;
            }
            else
            {
                fail(key("DIRECTION") + ": must be 'input' or 'output', got '" + *dir + "'");
            }

            auto const subdev = env.get(key("SUBDEVICE_INDEX"));
            if (!subdev)
            {
                fail(key("SUBDEVICE_INDEX") + " is required");
            }
            ch.subdeviceIndex = parseInt(key("SUBDEVICE_INDEX"), *subdev, 0, 7);

            if (auto const v = env.get(key("VIDEO_MODE")))
            {
                ch.videoModeName = *v;
            }
            if (ch.videoModeName != "auto")
            {
                ch.videoMode = lookupVideoMode(ch.videoModeName);
                if (!ch.videoMode)
                {
                    fail(key("VIDEO_MODE") + ": unknown video mode '" + ch.videoModeName + "'");
                }
            }
            else if (ch.direction == Direction::Output)
            {
                // §4.3: auto is rejected on output channels.
                fail(key("VIDEO_MODE") + ": 'auto' is not valid for output channels");
            }

            if (auto const v = env.get(key("PIXEL_FORMAT")))
            {
                if (*v == "10BitYUV")
                {
                    ch.pixelFormat = PixelFormat::YUV10;
                }
                else if (*v == "10BitYUVA")
                {
                    ch.pixelFormat = PixelFormat::YUVA10;
                }
                else if (*v == "8BitYUV")
                {
                    ch.pixelFormat = PixelFormat::YUV8;
                }
                else
                {
                    // §3.3: everything else (RGB…) is incompatible with MXL v1.0.
                    fail(key("PIXEL_FORMAT") + ": unsupported pixel format '" + *v + "' (MXL v1.0 supports 10BitYUV, 10BitYUVA, 8BitYUV)");
                }
            }

            if (auto const v = env.get(key("ALLOW_FORMAT_CONVERSION")))
            {
                ch.allowFormatConversion = parseBool(key("ALLOW_FORMAT_CONVERSION"), *v);
            }
            if (ch.pixelFormat == PixelFormat::YUV8 && !ch.allowFormatConversion)
            {
                fail(key("PIXEL_FORMAT") + ": 8BitYUV requires " + key("ALLOW_FORMAT_CONVERSION") + "=true (§3.3)");
            }

            if (auto const v = env.get(key("VIDEO_ANC_ENABLE")))
            {
                ch.ancEnable = parseBool(key("VIDEO_ANC_ENABLE"), *v);
            }
            if (auto const v = env.get(key("AUDIO_ENABLE")))
            {
                ch.audioEnable = parseBool(key("AUDIO_ENABLE"), *v);
            }
            if (auto const v = env.get(key("AUDIO_CHANNEL_COUNT")))
            {
                int const n = parseInt(key("AUDIO_CHANNEL_COUNT"), *v, 2, 64);
                if (n != 2 && n != 8 && n != 16 && n != 32 && n != 64)
                {
                    fail(key("AUDIO_CHANNEL_COUNT") + ": must be one of 2, 8, 16, 32, 64");
                }
                ch.audioChannelCount = n;
            }
            if (auto const v = env.get(key("AUDIO_SAMPLE_TYPE")))
            {
                if (*v == "16bit")
                {
                    ch.audioSampleType = AudioSampleType::Int16;
                }
                else if (*v == "32bit")
                {
                    ch.audioSampleType = AudioSampleType::Int32;
                }
                else
                {
                    fail(key("AUDIO_SAMPLE_TYPE") + ": must be '16bit' or '32bit'");
                }
            }

            auto const videoFlow = env.get(key("MXL_VIDEO_FLOW_ID"));
            if (!videoFlow)
            {
                fail(key("MXL_VIDEO_FLOW_ID") + " is required");
            }
            ch.videoFlowId = parseUuidOrFail(key("MXL_VIDEO_FLOW_ID"), *videoFlow);

            if (auto const v = env.get(key("MXL_AUDIO_FLOW_ID")))
            {
                ch.audioFlowId = parseUuidOrFail(key("MXL_AUDIO_FLOW_ID"), *v);
            }
            else if (ch.audioEnable)
            {
                fail(key("MXL_AUDIO_FLOW_ID") + " is required when audio is enabled");
            }

            if (auto const v = env.get(key("MXL_ANC_FLOW_ID")))
            {
                ch.ancFlowId = parseUuidOrFail(key("MXL_ANC_FLOW_ID"), *v);
            }
            else if (ch.ancEnable)
            {
                fail(key("MXL_ANC_FLOW_ID") + " is required when ANC is enabled");
            }

            if (auto const v = env.get(key("MXL_VIDEO_FLOW_LABEL")))
            {
                ch.videoFlowLabel = *v;
            }
            if (auto const v = env.get(key("MXL_AUDIO_FLOW_LABEL")))
            {
                ch.audioFlowLabel = *v;
            }
            if (auto const v = env.get(key("MXL_GROUP_HINT")))
            {
                ch.groupHint = *v;
            }
            if (auto const v = env.get(key("MXL_DEVICE_ID")))
            {
                ch.deviceId = parseUuidOrFail(key("MXL_DEVICE_ID"), *v);
            }
            if (auto const v = env.get(key("MXL_SOURCE_ID")))
            {
                ch.sourceId = parseUuidOrFail(key("MXL_SOURCE_ID"), *v);
            }

            if (auto const v = env.get(key("GRAIN_COUNT")))
            {
                ch.grainCount = parseInt(key("GRAIN_COUNT"), *v, 2, 1024);
            }
            if (auto const v = env.get(key("AUDIO_BUFFER_MS")))
            {
                ch.audioBufferMs = parseInt(key("AUDIO_BUFFER_MS"), *v, 10, 10000);
            }
            if (auto const v = env.get(key("COMMIT_BATCH_HINT")))
            {
                int const hint = parseInt(key("COMMIT_BATCH_HINT"), *v, 1, 1 << 20);
                ch.commitBatchHintVideo = hint;
                ch.commitBatchHintAudio = hint;
            }
            if (auto const v = env.get(key("OUTPUT_PREROLL_GRAINS")))
            {
                ch.outputPrerollGrains = parseInt(key("OUTPUT_PREROLL_GRAINS"), *v, 1, 64);
            }
            if (auto const v = env.get(key("READER_TIMEOUT_MS")))
            {
                ch.readerTimeoutMs = parseInt(key("READER_TIMEOUT_MS"), *v, 1, 10000);
            }
            if (auto const v = env.get(key("LABEL")))
            {
                ch.label = *v;
            }
            else
            {
                ch.label = "ch" + std::to_string(index);
            }
            if (ch.groupHint.empty())
            {
                ch.groupHint = ch.label;
            }

            return ch;
        }

        /// §4.4: maps legacy variable names onto a CH0_-style channel parse.
        ChannelConfig parseLegacyChannel(Env const& env)
        {
            std::unordered_map<std::string, std::string> const remap = {
                {"CH0_DIRECTION", "DIRECTION"},
                {"CH0_VIDEO_MODE", "VIDEO_MODE"},
                {"CH0_PIXEL_FORMAT", "PIXEL_FORMAT"},
                {"CH0_ALLOW_FORMAT_CONVERSION", "ALLOW_FORMAT_CONVERSION"},
                {"CH0_VIDEO_ANC_ENABLE", "VIDEO_ANC_ENABLE"},
                {"CH0_AUDIO_ENABLE", "AUDIO_ENABLE"},
                {"CH0_AUDIO_CHANNEL_COUNT", "AUDIO_CHANNEL_COUNT"},
                {"CH0_AUDIO_SAMPLE_TYPE", "AUDIO_SAMPLE_TYPE"},
                {"CH0_MXL_VIDEO_FLOW_ID", "MXL_VIDEO_FLOW_ID"},
                {"CH0_MXL_AUDIO_FLOW_ID", "MXL_AUDIO_FLOW_ID"},
                {"CH0_MXL_ANC_FLOW_ID", "MXL_ANC_FLOW_ID"},
                {"CH0_MXL_VIDEO_FLOW_LABEL", "MXL_VIDEO_FLOW_LABEL"},
                {"CH0_MXL_AUDIO_FLOW_LABEL", "MXL_AUDIO_FLOW_LABEL"},
                {"CH0_MXL_GROUP_HINT", "MXL_GROUP_HINT"},
                {"CH0_MXL_DEVICE_ID", "MXL_DEVICE_ID"},
                {"CH0_MXL_SOURCE_ID", "MXL_SOURCE_ID"},
                {"CH0_GRAIN_COUNT", "GRAIN_COUNT"},
                {"CH0_AUDIO_BUFFER_MS", "AUDIO_BUFFER_MS"},
                {"CH0_COMMIT_BATCH_HINT", "COMMIT_BATCH_HINT"},
                {"CH0_OUTPUT_PREROLL_GRAINS", "OUTPUT_PREROLL_GRAINS"},
                {"CH0_READER_TIMEOUT_MS", "READER_TIMEOUT_MS"},
                {"CH0_LABEL", "CHANNEL_LABEL"},
                // The legacy device selector addresses one sub-device
                // directly; the card match resolves it to sub-device 0 of the
                // matched (sub-)device.
                {"CH0_SUBDEVICE_INDEX", "__LEGACY_SUBDEVICE__"},
            };

            EnvReader const remappedReader = [&env, &remap](std::string const& name) -> std::optional<std::string> {
                auto const it = remap.find(name);
                if (it == remap.end())
                {
                    return std::nullopt;
                }
                if (it->second == "__LEGACY_SUBDEVICE__")
                {
                    return std::string("0");
                }
                return env.get(it->second);
            };
            Env const remappedEnv{remappedReader};
            return parseChannel(remappedEnv, 0, "CH0_");
        }

        void validateCrossChannel(Config const& cfg)
        {
            // §4.3: same sub-device index at most once per direction.
            std::set<std::pair<int, int>> seen;
            for (auto const& ch : cfg.channels)
            {
                auto const kdir = ch.direction == Direction::Input ? 0 : 1;
                if (!seen.insert({ch.subdeviceIndex, kdir}).second)
                {
                    fail("sub-device index " + std::to_string(ch.subdeviceIndex) + " used more than once for direction " +
                         directionName(ch.direction));
                }
            }

            // §4.3: flow UUIDs must be unique per producing/consuming role.
            // Input channels *create* flows, so their UUIDs must never
            // collide with any other channel of the same role; an output
            // channel may deliberately read a flow that an input channel of
            // the same container writes (loopback), so reader UUIDs are only
            // checked against other readers.
            auto checkUnique = [](std::set<std::string>& uuids, util::Uuid const& id, std::string const& what) {
                if (!uuids.insert(id.toString()).second)
                {
                    fail("duplicate flow UUID " + id.toString() + " (" + what + "); flow UUIDs must be unique per container");
                }
            };
            std::set<std::string> writerUuids;
            std::set<std::string> readerUuids;
            for (auto const& ch : cfg.channels)
            {
                auto& uuids = ch.direction == Direction::Input ? writerUuids : readerUuids;
                std::string const chName = "channel " + std::to_string(ch.index);
                checkUnique(uuids, ch.videoFlowId, chName + " video");
                if (ch.audioFlowId)
                {
                    checkUnique(uuids, *ch.audioFlowId, chName + " audio");
                }
                if (ch.ancFlowId)
                {
                    checkUnique(uuids, *ch.ancFlowId, chName + " anc");
                }
            }
        }
    }

    int ChannelConfig::effectiveGrainCount() const
    {
        if (grainCount > 0)
        {
            return grainCount;
        }
        // §4.2 default: 12 (HD) / 8 (UHD and larger).
        if (videoMode && videoMode->width > 1920)
        {
            return 8;
        }
        return 12;
    }

    EnvReader systemEnv()
    {
        return [](std::string const& name) -> std::optional<std::string> {
            char const* const v = std::getenv(name.c_str());
            if (v == nullptr)
            {
                return std::nullopt;
            }
            return std::string(v);
        };
    }

    char const* directionName(Direction d)
    {
        return d == Direction::Input ? "input" : "output";
    }

    char const* profileName(CardProfile p)
    {
        switch (p)
        {
            case CardProfile::OneFullDuplex: return "one-full-duplex";
            case CardProfile::TwoHalfDuplex: return "two-half-duplex";
            case CardProfile::FourHalfDuplex: return "four-half-duplex";
            case CardProfile::OneHalfDuplex: return "one-half-duplex";
        }
        return "?";
    }

    Config loadConfig(EnvReader const& reader)
    {
        Env const env{reader};
        Config cfg;

        auto const channelIndices = presentChannelIndices(env);
        auto const legacyPresent = presentLegacyVars(env);

        // §4.4: mixing v1.0 and v1.1 variables is a hard error.
        if (!channelIndices.empty() && !legacyPresent.empty())
        {
            fail("mixed v1.0 (single-channel, e.g. " + legacyPresent.front() + ") and v1.1 (CHx_) configuration variables; use one style only");
        }

        // Card selection: v1.1 names take precedence, legacy names accepted
        // in legacy mode.
        bool const legacyMode = channelIndices.empty() && !legacyPresent.empty();
        cfg.legacyMode = legacyMode;

        if (auto const v = env.get("MXL_DECKLINK_CARD_ID"))
        {
            cfg.cardId = parseHex32("MXL_DECKLINK_CARD_ID", *v);
        }
        else if (auto const lv = env.get("DECKLINK_DEVICE_ID"); lv && legacyMode)
        {
            cfg.cardId = parseHex32("DECKLINK_DEVICE_ID", *lv);
        }
        if (auto const v = env.get("MXL_DECKLINK_CARD_NAME"))
        {
            cfg.cardName = *v;
        }
        else if (auto const lv = env.get("DECKLINK_DEVICE_NAME"); lv && legacyMode)
        {
            cfg.cardName = *lv;
        }
        if (auto const v = env.get("MXL_DECKLINK_CARD_INDEX"))
        {
            cfg.cardIndex = parseInt("MXL_DECKLINK_CARD_INDEX", *v, 0, 63);
        }
        else if (auto const lv = env.get("DECKLINK_DEVICE_INDEX"); lv && legacyMode)
        {
            cfg.cardIndex = parseInt("DECKLINK_DEVICE_INDEX", *lv, 0, 63);
        }

        int const selectors = (cfg.cardId ? 1 : 0) + (cfg.cardName ? 1 : 0) + (cfg.cardIndex ? 1 : 0);
        if (selectors == 0)
        {
            fail("one of MXL_DECKLINK_CARD_ID, MXL_DECKLINK_CARD_NAME, MXL_DECKLINK_CARD_INDEX is required");
        }
        if (selectors > 1)
        {
            fail("only one of MXL_DECKLINK_CARD_ID, MXL_DECKLINK_CARD_NAME, MXL_DECKLINK_CARD_INDEX may be set");
        }

        if (auto const v = env.get("MXL_DECKLINK_CARD_PROFILE"))
        {
            if (*v == "one-full-duplex")
            {
                cfg.cardProfile = CardProfile::OneFullDuplex;
            }
            else if (*v == "two-half-duplex")
            {
                cfg.cardProfile = CardProfile::TwoHalfDuplex;
            }
            else if (*v == "four-half-duplex")
            {
                cfg.cardProfile = CardProfile::FourHalfDuplex;
            }
            else if (*v == "one-half-duplex")
            {
                cfg.cardProfile = CardProfile::OneHalfDuplex;
            }
            else
            {
                fail("MXL_DECKLINK_CARD_PROFILE: unknown profile '" + *v + "'");
            }
        }

        if (auto const v = env.get("MXL_DOMAIN_PATH"))
        {
            cfg.domainPath = *v;
        }
        if (auto const v = env.get("MXL_TIMESTAMP_SOURCE"))
        {
            if (*v == "hardware")
            {
                cfg.timestampSource = TimestampSourceCfg::Hardware;
            }
            else if (*v == "host")
            {
                cfg.timestampSource = TimestampSourceCfg::Host;
            }
            else
            {
                fail("MXL_TIMESTAMP_SOURCE: must be 'hardware' or 'host'");
            }
        }
        if (auto const v = env.get("MXL_HUGEPAGE_PATH"))
        {
            cfg.hugepagePath = *v;
        }
        if (auto const v = env.get("MXL_CPU_PIN_LIST"))
        {
            cfg.cpuPinList = util::parseCpuList(*v);
            if (!cfg.cpuPinList)
            {
                fail("MXL_CPU_PIN_LIST: invalid CPU list '" + *v + "'");
            }
        }
        if (auto const v = env.get("MXL_REALTIME_PRIORITY"))
        {
            cfg.realtimePriority = parseInt("MXL_REALTIME_PRIORITY", *v, 1, 99);
        }
        if (auto const v = env.get("RT_SCHED"))
        {
            cfg.rtSched = parseBool("RT_SCHED", *v);
        }
        if (auto const v = env.get("MXL_PTP_INTERFACE"))
        {
            cfg.ptpInterface = *v;
        }
        if (auto const v = env.get("HEALTH_PORT"))
        {
            cfg.healthPort = parseInt("HEALTH_PORT", *v, 1, 65535);
        }
        if (auto const v = env.get("METRICS_PORT"))
        {
            cfg.metricsPort = parseInt("METRICS_PORT", *v, 1, 65535);
        }
        if (cfg.healthPort == cfg.metricsPort)
        {
            fail("HEALTH_PORT and METRICS_PORT must differ");
        }
        if (auto const v = env.get("MXL_HEALTH_MIN_HEALTHY_CHANNELS"))
        {
            cfg.minHealthyChannels = parseInt("MXL_HEALTH_MIN_HEALTHY_CHANNELS", *v, 0, kMaxChannels);
        }
        if (auto const v = env.get("SIGNAL_LOSS_TIMEOUT_S"))
        {
            cfg.signalLossTimeoutS = parseInt("SIGNAL_LOSS_TIMEOUT_S", *v, 1, 86400);
        }
        if (auto const v = env.get("STARTUP_MAX_RETRIES"))
        {
            cfg.startupMaxRetries = parseInt("STARTUP_MAX_RETRIES", *v, 0, 1000);
        }
        if (auto const v = env.get("SHUTDOWN_TIMEOUT_S"))
        {
            cfg.shutdownTimeoutS = parseInt("SHUTDOWN_TIMEOUT_S", *v, 1, 600);
        }
        if (auto const v = env.get("LOG_LEVEL"))
        {
            if (*v != "trace" && *v != "debug" && *v != "info" && *v != "warn" && *v != "error")
            {
                fail("LOG_LEVEL: must be one of trace/debug/info/warn/error");
            }
            cfg.logLevel = *v;
        }
        if (auto const v = env.get("LOG_FORMAT"))
        {
            if (*v != "json" && *v != "text")
            {
                fail("LOG_FORMAT: must be 'json' or 'text'");
            }
            cfg.logFormat = *v;
        }
        if (auto const v = env.get("DECKLINK_LIB_MODE"))
        {
            if (*v == "bundled")
            {
                cfg.libMode = DeckLinkLibMode::Bundled;
            }
            else if (*v == "hostmount")
            {
                cfg.libMode = DeckLinkLibMode::HostMount;
            }
            else
            {
                fail("DECKLINK_LIB_MODE: must be 'bundled' or 'hostmount'");
            }
        }
        if (auto const v = env.get("MXL_DECKLINK_BACKEND"))
        {
            if (*v != "sdk" && *v != "mock")
            {
                fail("MXL_DECKLINK_BACKEND: must be 'sdk' or 'mock'");
            }
            cfg.backend = *v;
        }

        // Channels.
        if (legacyMode)
        {
            cfg.channels.push_back(parseLegacyChannel(env));
        }
        else
        {
            if (channelIndices.empty())
            {
                fail("no channels configured: set CHx_DIRECTION for at least one channel (or use the legacy v1.0 variables)");
            }
            for (int const idx : channelIndices)
            {
                cfg.channels.push_back(parseChannel(env, idx, "CH" + std::to_string(idx) + "_"));
            }
        }

        validateCrossChannel(cfg);

        if (cfg.minHealthyChannels > static_cast<int>(cfg.channels.size()))
        {
            fail("MXL_HEALTH_MIN_HEALTHY_CHANNELS (" + std::to_string(cfg.minHealthyChannels) + ") exceeds the number of configured channels (" +
                 std::to_string(cfg.channels.size()) + ")");
        }

        return cfg;
    }
}
