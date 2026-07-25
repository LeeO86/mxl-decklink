// SPDX-License-Identifier: MIT
#include "store.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

#include <picojson/picojson.h>
#include <unistd.h>

#include "util/logging.hpp"

namespace mxldl::config
{
    namespace
    {
        /// Legacy v1.0 names are env-only (§4.5); the file layer must use the
        /// v1.1 schema exclusively.
        bool isEnvOnlyKey(std::string const& key)
        {
            return key == "MXL_CONFIG_FILE";
        }
    }

    char const* settingSourceName(SettingSource s)
    {
        switch (s)
        {
            case SettingSource::Default: return "default";
            case SettingSource::File: return "file";
            case SettingSource::Env: return "env";
        }
        return "?";
    }

    ConfigStore::ConfigStore(EnvReader env)
        : _env(std::move(env))
    {
        if (auto const p = _env("MXL_CONFIG_FILE"); p && !p->empty())
        {
            _filePath = *p;
            loadFile();
        }
    }

    void ConfigStore::loadFile()
    {
        _fileLayer.clear();
        if (!_filePath || !std::filesystem::exists(*_filePath))
        {
            // §4.5: a missing file is not an error; it is created on first save.
            return;
        }

        std::ifstream in(*_filePath);
        if (!in)
        {
            throw ConfigError("MXL_CONFIG_FILE: cannot read " + *_filePath);
        }
        picojson::value root;
        std::string const err = picojson::parse(root, in);
        if (!err.empty())
        {
            throw ConfigError("MXL_CONFIG_FILE: invalid JSON in " + *_filePath + ": " + err);
        }
        if (!root.is<picojson::object>())
        {
            throw ConfigError("MXL_CONFIG_FILE: top level must be a JSON object of KEY: \"value\" pairs");
        }
        for (auto const& [key, value] : root.get<picojson::object>())
        {
            if (!lookupSetting(key))
            {
                throw ConfigError("MXL_CONFIG_FILE: unknown configuration key '" + key + "' (the file uses the v1.1 CHx_/global names)");
            }
            if (isEnvOnlyKey(key))
            {
                throw ConfigError("MXL_CONFIG_FILE: key '" + key + "' cannot be set from the file");
            }
            std::string rendered;
            if (value.is<std::string>())
            {
                rendered = value.get<std::string>();
            }
            else if (value.is<bool>())
            {
                rendered = value.get<bool>() ? "true" : "false";
            }
            else if (value.is<double>())
            {
                double const d = value.get<double>();
                if (d == static_cast<double>(static_cast<long long>(d)))
                {
                    rendered = std::to_string(static_cast<long long>(d));
                }
                else
                {
                    throw ConfigError("MXL_CONFIG_FILE: key '" + key + "' must be a string/integer/bool");
                }
            }
            else
            {
                throw ConfigError("MXL_CONFIG_FILE: key '" + key + "' must be a string/integer/bool");
            }
            _fileLayer[key] = rendered;
        }
    }

    EnvReader ConfigStore::mergedReader(std::map<std::string, std::string> const& fileLayer) const
    {
        // §4.5 precedence: env over file. Empty env values count as unset
        // (mirrors the loader's ConfigMap-friendly behavior).
        auto const env = _env;
        return [env, fileLayer](std::string const& name) -> std::optional<std::string> {
            if (auto const v = env(name); v && !v->empty())
            {
                return v;
            }
            if (auto const it = fileLayer.find(name); it != fileLayer.end())
            {
                return it->second;
            }
            return std::nullopt;
        };
    }

    Config ConfigStore::effectiveConfig() const
    {
        std::lock_guard const lock{_mutex};
        return loadConfig(mergedReader(_fileLayer));
    }

    std::optional<std::string> ConfigStore::effectiveValue(std::string const& key) const
    {
        std::lock_guard const lock{_mutex};
        return mergedReader(_fileLayer)(key);
    }

    SettingSource ConfigStore::sourceOf(std::string const& key) const
    {
        std::lock_guard const lock{_mutex};
        if (auto const v = _env(key); v && !v->empty())
        {
            return SettingSource::Env;
        }
        if (_fileLayer.count(key) != 0)
        {
            return SettingSource::File;
        }
        return SettingSource::Default;
    }

    bool ConfigStore::hasFileLayer() const
    {
        return _filePath.has_value();
    }

    std::optional<std::string> ConfigStore::filePath() const
    {
        return _filePath;
    }

    std::vector<std::string> ConfigStore::presentKeys() const
    {
        std::lock_guard const lock{_mutex};
        std::map<std::string, bool> keys;
        for (auto const& [k, v] : _fileLayer)
        {
            keys[k] = true;
        }
        // Environment keys: probe every schema key (global + all channel
        // indices) — the env cannot be enumerated portably through EnvReader.
        for (auto const& meta : settingsSchema())
        {
            if (meta.kind == SettingKind::Global)
            {
                if (auto const v = _env(meta.key); v && !v->empty())
                {
                    keys[meta.key] = true;
                }
            }
            else
            {
                for (int i = 0; i < 16; ++i)
                {
                    auto const key = "CH" + std::to_string(i) + "_" + meta.key;
                    if (auto const v = _env(key); v && !v->empty())
                    {
                        keys[key] = true;
                    }
                }
            }
        }
        std::vector<std::string> out;
        out.reserve(keys.size());
        for (auto const& [k, v] : keys)
        {
            out.push_back(k);
        }
        return out;
    }

    std::variant<ConfigStore::UpdateResult, std::string> ConfigStore::update(std::map<std::string, std::optional<std::string>> const& changes)
    {
        std::lock_guard const lock{_mutex};
        if (!_filePath)
        {
            return std::string("no configuration file configured (set MXL_CONFIG_FILE and mount a config volume) — settings cannot be persisted");
        }

        // Validate the individual keys first.
        for (auto const& [key, value] : changes)
        {
            if (!lookupSetting(key))
            {
                return "unknown configuration key '" + key + "'";
            }
            if (isEnvOnlyKey(key))
            {
                return "key '" + key + "' cannot be set from the file layer";
            }
            if (auto const v = _env(key); v && !v->empty())
            {
                // §7.5.2: env-supplied settings are immutable through the API.
                return "key '" + key + "' is set via environment variable and cannot be changed here";
            }
        }

        // Build the candidate file layer and validate the merged result.
        auto candidate = _fileLayer;
        std::vector<std::string> changedKeys;
        for (auto const& [key, value] : changes)
        {
            auto const it = candidate.find(key);
            if (value)
            {
                if (it == candidate.end() || it->second != *value)
                {
                    candidate[key] = *value;
                    changedKeys.push_back(key);
                }
            }
            else if (it != candidate.end())
            {
                candidate.erase(it);
                changedKeys.push_back(key);
            }
        }

        Config newConfig;
        try
        {
            newConfig = loadConfig(mergedReader(candidate));
        }
        catch (ConfigError const& e)
        {
            return std::string(e.what());
        }

        // Persist atomically (§4.5): temp file + rename.
        {
            std::string json = "{\n";
            bool first = true;
            for (auto const& [key, value] : candidate)
            {
                if (!first)
                {
                    json += ",\n";
                }
                first = false;
                json += "  \"" + log::jsonEscape(key) + "\": \"" + log::jsonEscape(value) + "\"";
            }
            json += "\n}\n";

            auto const dir = std::filesystem::path(*_filePath).parent_path();
            if (!dir.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
            }
            auto const tmp = *_filePath + ".tmp";
            {
                std::ofstream out(tmp, std::ios::trunc);
                if (!out)
                {
                    return "cannot write configuration file " + tmp + " (is the config volume mounted writable?)";
                }
                out << json;
                out.flush();
                if (!out)
                {
                    return "short write to " + tmp;
                }
            }
            std::error_code ec;
            std::filesystem::rename(tmp, *_filePath, ec);
            if (ec)
            {
                return "cannot replace " + *_filePath + ": " + ec.message();
            }
        }

        _fileLayer = std::move(candidate);
        log::info("config_file_updated",
            {
                {"path", *_filePath},
                {"changed_keys", static_cast<std::uint64_t>(changedKeys.size())},
            });
        return UpdateResult{std::move(newConfig), std::move(changedKeys)};
    }

    std::string ConfigStore::renderEnvBlock() const
    {
        auto const keys = presentKeys();
        std::lock_guard const lock{_mutex};
        auto const reader = mergedReader(_fileLayer);
        std::string out;
        for (auto const& key : keys)
        {
            if (auto const v = reader(key))
            {
                out += key + "=" + *v + "\n";
            }
        }
        return out;
    }
}
