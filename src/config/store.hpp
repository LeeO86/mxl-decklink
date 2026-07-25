// SPDX-License-Identifier: MIT
// Layered configuration store (SPECIFICATION.md §4.5):
// environment > file (MXL_CONFIG_FILE, flat env-var-keyed JSON) > default.
#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <variant>

#include "config/config.hpp"
#include "config/schema.hpp"

namespace mxldl::config
{
    enum class SettingSource
    {
        Default,
        File,
        Env,
    };

    char const* settingSourceName(SettingSource s);

    class ConfigStore
    {
    public:
        /// Reads MXL_CONFIG_FILE from `env` and loads the file layer when the
        /// variable is set and the file exists. Throws ConfigError for a
        /// malformed/unknown-key file (startup: exit 78 per §4.5).
        explicit ConfigStore(EnvReader env);

        /// Effective, fully validated configuration (env over file over
        /// defaults). Throws ConfigError when the merge is invalid.
        [[nodiscard]] Config effectiveConfig() const;

        /// Merged raw value of one key (nullopt = built-in default applies).
        [[nodiscard]] std::optional<std::string> effectiveValue(std::string const& key) const;

        [[nodiscard]] SettingSource sourceOf(std::string const& key) const;

        /// True when a config file path is configured (whether or not the
        /// file exists yet).
        [[nodiscard]] bool hasFileLayer() const;
        [[nodiscard]] std::optional<std::string> filePath() const;

        /// All keys currently present in either layer (sorted).
        [[nodiscard]] std::vector<std::string> presentKeys() const;

        struct UpdateResult
        {
            Config config; // the new effective configuration
            std::vector<std::string> changedKeys;
        };

        /// Applies changes to the FILE layer: `value` set → upsert, nullopt →
        /// remove. Env-supplied keys are rejected (§7.5.2), unknown keys are
        /// rejected, and the merged result must pass full validation before
        /// the file is atomically rewritten. Returns the error message on
        /// rejection (nothing persisted / changed).
        std::variant<UpdateResult, std::string> update(std::map<std::string, std::optional<std::string>> const& changes);

        /// Effective configuration rendered as a copyable KEY=value block
        /// (§7.5.1 Settings tab).
        [[nodiscard]] std::string renderEnvBlock() const;

    private:
        void loadFile();
        [[nodiscard]] EnvReader mergedReader(std::map<std::string, std::string> const& fileLayer) const;

        EnvReader _env;
        std::optional<std::string> _filePath;
        mutable std::mutex _mutex;
        std::map<std::string, std::string> _fileLayer;
    };
}
