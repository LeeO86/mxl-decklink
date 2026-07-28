// SPDX-License-Identifier: MIT
// Configuration schema metadata (SPECIFICATION.md §4.1/§4.2) powering the
// web interface forms and the /api/config endpoint (§7.5).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mxldl::config
{
    enum class SettingKind
    {
        Global,
        Channel, // key is the CHx_ suffix, e.g. "DIRECTION"
        AudioFlow, // template key AFn_* (FLOW_ID / CHANNEL_COUNT / MAP / LABEL)
    };

    struct SettingMeta
    {
        std::string key; // global name, channel suffix, or audio-flow template ("FLOW_ID")
        SettingKind kind = SettingKind::Global;
        std::string type; // "string","int","bool","enum","uuid","port","path","hex32","cpulist","intlist"
        std::vector<std::string> options; // for type == "enum"
        std::string defaultValue; // rendered default ("" = none)
        std::string help;
        bool requiresRestart = false; // global keys that only apply on process start
    };

    /// The complete schema (legacy v1.0 names are accepted by the loader but
    /// not part of the editable schema).
    std::vector<SettingMeta> const& settingsSchema();

    /// Looks up metadata for a concrete key ("MXL_DOMAIN_PATH", "CH3_DIRECTION",
    /// "CH0_AF2_MAP"). Returns nullopt for unknown keys.
    std::optional<SettingMeta> lookupSetting(std::string_view key);

    /// Splits "CH<idx>_<suffix>"; returns nullopt when not a channel key.
    std::optional<std::pair<int, std::string>> parseChannelKey(std::string_view key);

    /// Splits "AF<n>_<field>" from a channel suffix; field is FLOW_ID /
    /// CHANNEL_COUNT / MAP / LABEL. Returns nullopt when not an audio-flow key.
    std::optional<std::pair<int, std::string>> parseAudioFlowSuffix(std::string_view suffix);
}
