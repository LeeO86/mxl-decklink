// SPDX-License-Identifier: MIT
#include "schema.hpp"

#include <cstdlib>

#include "config/videomodes.hpp"

namespace mxldl::config
{
    namespace
    {
        std::vector<SettingMeta> buildSchema()
        {
            std::vector<std::string> videoModes = {"auto"};
            for (auto const& m : allVideoModes())
            {
                videoModes.push_back(m.name);
            }

            return {
                // --- Global (§4.1) -------------------------------------------------
                {"MXL_DECKLINK_CARD_ID", SettingKind::Global, "hex32", {}, "",
                    "BMDDeckLinkPersistentID of the card (hex, e.g. 0xa1b2c3d4). Stable across reboots; recommended selector.", true},
                {"MXL_DECKLINK_CARD_NAME", SettingKind::Global, "string", {}, "",
                    "Card display name selector (lab use; not reboot-stable).", true},
                {"MXL_DECKLINK_CARD_INDEX", SettingKind::Global, "int", {}, "",
                    "Zero-based card index selector (lab use; not reboot-stable).", true},
                {"MXL_DECKLINK_CARD_PROFILE", SettingKind::Global, "enum",
                    {"one-full-duplex", "two-half-duplex", "four-half-duplex", "one-half-duplex"}, "",
                    "Card profile applied at startup (SDI cards only).", true},
                {"MXL_DOMAIN_PATH", SettingKind::Global, "path", {}, "/dev/shm/mxl",
                    "MXL domain directory (must be tmpfs-backed).", true},
                {"MXL_TIMESTAMP_SOURCE", SettingKind::Global, "enum", {"hardware", "host"}, "hardware",
                    "Frame timestamp source (§3.5).", true},
                {"MXL_HUGEPAGE_PATH", SettingKind::Global, "path", {}, "", "Optional HugePages mount for grain buffers.", true},
                {"MXL_CPU_PIN_LIST", SettingKind::Global, "cpulist", {}, "",
                    "CPU list for streaming-thread pinning (outside Kubernetes only).", true},
                {"MXL_REALTIME_PRIORITY", SettingKind::Global, "int", {}, "50", "SCHED_FIFO priority (1-99); requires RT_SCHED.", true},
                {"RT_SCHED", SettingKind::Global, "bool", {}, "false", "Enable SCHED_FIFO for streaming threads (needs CAP_SYS_NICE).", true},
                {"MXL_PTP_INTERFACE", SettingKind::Global, "string", {}, "", "PTP status correlation interface (informational).", true},
                {"HEALTH_PORT", SettingKind::Global, "port", {}, "9080", "HTTP port for /livez /readyz /statusz.", true},
                {"METRICS_PORT", SettingKind::Global, "port", {}, "9090", "HTTP port for /metrics.", true},
                {"WEB_ENABLE", SettingKind::Global, "bool", {}, "true", "Enable the web interface and REST API (§7.5).", true},
                {"WEB_PORT", SettingKind::Global, "port", {}, "8080", "HTTP port for the web interface.", true},
                {"MXL_CONFIG_FILE", SettingKind::Global, "path", {}, "", "JSON configuration file (§4.5). Env-only setting.", true},
                {"MXL_DOMAIN_SCAN_PATH", SettingKind::Global, "path", {}, "/dev/shm", "Root directory scanned for MXL domains (§7.6).", true},
                {"MXL_HEALTH_MIN_HEALTHY_CHANNELS", SettingKind::Global, "int", {}, "1", "Readiness threshold (§7.2).", true},
                {"SIGNAL_LOSS_TIMEOUT_S", SettingKind::Global, "int", {}, "30", "Window without signal before a stream reset cycle (§3.6).", true},
                {"STARTUP_MAX_RETRIES", SettingKind::Global, "int", {}, "10", "Card-level startup retries (§3.10).", true},
                {"SHUTDOWN_TIMEOUT_S", SettingKind::Global, "int", {}, "10", "SIGTERM grace period (§3.10).", true},
                {"LOG_LEVEL", SettingKind::Global, "enum", {"trace", "debug", "info", "warn", "error"}, "info", "Log level.", true},
                {"LOG_FORMAT", SettingKind::Global, "enum", {"json", "text"}, "json", "Log format.", true},
                {"DECKLINK_LIB_MODE", SettingKind::Global, "enum", {"bundled", "hostmount"}, "bundled",
                    "Documentation of the libDeckLinkAPI.so provisioning pattern (§5.1).", true},
                {"MXL_DECKLINK_BACKEND", SettingKind::Global, "enum", {"sdk", "mock"}, "sdk", "Testing facility: 'mock' uses the software card.",
                    true},

                // --- Per channel (§4.2), keyed by suffix ---------------------------
                {"DIRECTION", SettingKind::Channel, "enum", {"input", "output"}, "",
                    "Channel direction; presence activates the channel.", false},
                {"SUBDEVICE_INDEX", SettingKind::Channel, "int", {}, "", "Zero-based sub-device index within the card.", false},
                {"VIDEO_MODE", SettingKind::Channel, "enum", videoModes, "auto",
                    "'auto' (input format detection, inputs only) or an explicit mode (§3.2.1).", false},
                {"PIXEL_FORMAT", SettingKind::Channel, "enum", {"10BitYUV", "10BitYUVA", "8BitYUV"}, "10BitYUV", "DeckLink pixel format (§3.3).",
                    false},
                {"ALLOW_FORMAT_CONVERSION", SettingKind::Channel, "bool", {}, "false", "Permit 8-bit → v210 expansion (§3.3).", false},
                {"VIDEO_ANC_ENABLE", SettingKind::Channel, "bool", {}, "false", "Create an additional video/smpte291 ANC flow.", false},
                {"AUDIO_ENABLE", SettingKind::Channel, "bool", {}, "true", "Enable the audio flow.", false},
                {"AUDIO_CHANNEL_COUNT", SettingKind::Channel, "enum", {"2", "8", "16", "32", "64"}, "16", "DeckLink audio channel count.", false},
                {"AUDIO_SAMPLE_TYPE", SettingKind::Channel, "enum", {"16bit", "32bit"}, "32bit", "DeckLink audio sample type.", false},
                {"MXL_VIDEO_FLOW_ID", SettingKind::Channel, "uuid", {}, "", "Video flow UUID (stable, managed externally).", false},
                {"MXL_AUDIO_FLOW_ID", SettingKind::Channel, "uuid", {}, "", "Audio flow UUID (required when audio enabled).", false},
                {"MXL_ANC_FLOW_ID", SettingKind::Channel, "uuid", {}, "", "ANC flow UUID (required when ANC enabled).", false},
                {"MXL_VIDEO_FLOW_LABEL", SettingKind::Channel, "string", {}, "", "Human-readable video flow label.", false},
                {"MXL_AUDIO_FLOW_LABEL", SettingKind::Channel, "string", {}, "", "Human-readable audio flow label.", false},
                {"MXL_GROUP_HINT", SettingKind::Channel, "string", {}, "", "NMOS grouphint value (defaults to the channel label).", false},
                {"MXL_DEVICE_ID", SettingKind::Channel, "uuid", {}, "", "device_id in the flow descriptors.", false},
                {"MXL_SOURCE_ID", SettingKind::Channel, "uuid", {}, "", "source_id in the flow descriptors.", false},
                {"GRAIN_COUNT", SettingKind::Channel, "int", {}, "", "Requested video ring depth in grains (12 HD / 8 UHD default).", false},
                {"AUDIO_BUFFER_MS", SettingKind::Channel, "int", {}, "200", "Requested audio ring length in ms.", false},
                {"COMMIT_BATCH_HINT", SettingKind::Channel, "int", {}, "", "maxCommitBatchSizeHint for the flow writers.", false},
                {"OUTPUT_PREROLL_GRAINS", SettingKind::Channel, "int", {}, "3", "Output channels: grains buffered before playback start.", false},
                {"READER_TIMEOUT_MS", SettingKind::Channel, "int", {}, "50", "Output channels: MXL grain read timeout.", false},
                {"LABEL", SettingKind::Channel, "string", {}, "", "Channel label for logs/metrics (defaults to ch<x>).", false},
            };
        }
    }

    std::vector<SettingMeta> const& settingsSchema()
    {
        static std::vector<SettingMeta> const schema = buildSchema();
        return schema;
    }

    std::optional<std::pair<int, std::string>> parseChannelKey(std::string_view key)
    {
        if (key.size() < 4 || key.substr(0, 2) != "CH")
        {
            return std::nullopt;
        }
        std::size_t i = 2;
        int idx = 0;
        bool haveDigit = false;
        while (i < key.size() && key[i] >= '0' && key[i] <= '9')
        {
            idx = idx * 10 + (key[i] - '0');
            haveDigit = true;
            ++i;
        }
        if (!haveDigit || i >= key.size() || key[i] != '_' || idx > 15)
        {
            return std::nullopt;
        }
        return std::make_pair(idx, std::string(key.substr(i + 1)));
    }

    std::optional<SettingMeta> lookupSetting(std::string_view key)
    {
        if (auto const ch = parseChannelKey(key))
        {
            for (auto const& m : settingsSchema())
            {
                if (m.kind == SettingKind::Channel && m.key == ch->second)
                {
                    return m;
                }
            }
            return std::nullopt;
        }
        for (auto const& m : settingsSchema())
        {
            if (m.kind == SettingKind::Global && m.key == key)
            {
                return m;
            }
        }
        return std::nullopt;
    }
}
