// SPDX-License-Identifier: MIT
// Environment-variable configuration schema per SPECIFICATION.md §4.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "config/videomodes.hpp"
#include "util/uuid.hpp"

namespace mxldl::config
{
    /// Thrown for any invalid configuration; main() maps it to exit 78
    /// (EX_CONFIG) with a structured error log entry (§4.3).
    class ConfigError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    enum class Direction
    {
        Input,
        Output,
    };

    enum class PixelFormat
    {
        YUV10, // bmdFormat10BitYUV → video/v210 (default)
        YUVA10, // fill+key → video/v210a
        YUV8, // bmdFormat8BitYUV, requires CHx_ALLOW_FORMAT_CONVERSION
    };

    enum class AudioSampleType
    {
        Int16,
        Int32,
    };

    enum class TimestampSourceCfg
    {
        Hardware,
        Host,
    };

    enum class CardProfile
    {
        OneFullDuplex,
        TwoHalfDuplex,
        FourHalfDuplex,
        OneHalfDuplex,
    };

    enum class DeckLinkLibMode
    {
        Bundled,
        HostMount,
    };

    struct ChannelConfig
    {
        int index = 0; // the x in CHx_
        Direction direction = Direction::Input;
        int subdeviceIndex = 0;
        std::string videoModeName = "auto"; // "auto" or table name
        std::optional<VideoMode> videoMode; // resolved when not auto
        PixelFormat pixelFormat = PixelFormat::YUV10;
        bool allowFormatConversion = false;
        bool ancEnable = false;
        bool audioEnable = true;
        int audioChannelCount = 16;
        AudioSampleType audioSampleType = AudioSampleType::Int32;

        util::Uuid videoFlowId{};
        std::optional<util::Uuid> audioFlowId;
        std::optional<util::Uuid> ancFlowId;
        std::string videoFlowLabel; // defaulted at validation
        std::string audioFlowLabel;
        std::string groupHint;
        std::optional<util::Uuid> deviceId;
        std::optional<util::Uuid> sourceId;

        int grainCount = 0; // 0 = derive default (12 HD / 8 UHD)
        int audioBufferMs = 200;
        int commitBatchHintVideo = 1;
        int commitBatchHintAudio = 256;
        int outputPrerollGrains = 3;
        int readerTimeoutMs = 50;
        std::string label; // defaulted to "ch<x>"

        [[nodiscard]] bool isAutoMode() const
        {
            return videoModeName == "auto";
        }

        [[nodiscard]] int effectiveGrainCount() const;
    };

    struct Config
    {
        // Card selection (§4.1) — exactly one of the three is set.
        std::optional<std::uint32_t> cardId; // BMDDeckLinkPersistentID
        std::optional<std::string> cardName;
        std::optional<int> cardIndex;

        std::optional<CardProfile> cardProfile;
        std::string domainPath = "/dev/shm/mxl";
        TimestampSourceCfg timestampSource = TimestampSourceCfg::Hardware;
        std::optional<std::string> hugepagePath;
        std::optional<std::vector<int>> cpuPinList;
        int realtimePriority = 50;
        bool rtSched = false;
        std::optional<std::string> ptpInterface;
        int healthPort = 9080;
        int metricsPort = 9090;
        int minHealthyChannels = 1;
        int signalLossTimeoutS = 30;
        int startupMaxRetries = 10;
        int shutdownTimeoutS = 10;
        std::string logLevel = "info";
        std::string logFormat = "json";
        DeckLinkLibMode libMode = DeckLinkLibMode::Bundled;

        // Testing facility (documented in README, not in the spec): "sdk" or "mock".
        std::string backend = "sdk";

        std::vector<ChannelConfig> channels;

        /// True when the configuration was assembled from legacy v1.0
        /// single-channel variables (§4.4).
        bool legacyMode = false;
    };

    /// Environment accessor indirection so tests can inject variables.
    using EnvReader = std::function<std::optional<std::string>(std::string const&)>;

    /// Reads the real process environment.
    EnvReader systemEnv();

    /// Parses and validates the full configuration (§4.1–§4.4).
    /// Throws ConfigError on any violation.
    Config loadConfig(EnvReader const& env);

    char const* directionName(Direction d);
    char const* profileName(CardProfile p);
}
