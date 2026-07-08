// SPDX-License-Identifier: MIT
// Structured logging per SPECIFICATION.md §7.4.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace mxldl::log
{
    enum class Level : int
    {
        Trace = 0,
        Debug = 1,
        Info = 2,
        Warn = 3,
        Error = 4,
    };

    enum class Format
    {
        Json,
        Text,
    };

    /// One `key: value` pair attached to a log event. Values are pre-rendered
    /// to strings; `isRaw` marks values that must be emitted without JSON
    /// string quoting (numbers, booleans, nested objects).
    struct Field
    {
        std::string key;
        std::string value;
        bool isRaw = false;

        Field(std::string k, std::string v)
            : key(std::move(k))
            , value(std::move(v))
        {}

        Field(std::string k, char const* v)
            : key(std::move(k))
            , value(v)
        {}

        Field(std::string k, std::string_view v)
            : key(std::move(k))
            , value(v)
        {}

        Field(std::string k, bool v)
            : key(std::move(k))
            , value(v ? "true" : "false")
            , isRaw(true)
        {}

        template<typename T>
            requires(std::is_integral_v<T> || std::is_floating_point_v<T>)
        Field(std::string k, T v)
            : key(std::move(k))
            , value(std::to_string(v))
            , isRaw(true)
        {}
    };

    void configure(Level level, Format format);
    Level parseLevel(std::string_view s); // throws std::invalid_argument
    Format parseFormat(std::string_view s); // throws std::invalid_argument
    Level currentLevel();

    /// Emit one structured event. `event` is a short machine-readable name
    /// (e.g. "signal_lost"); fields carry the context (§7.4 field set).
    void write(Level level, std::string_view event, std::initializer_list<Field> fields = {});

    void trace(std::string_view event, std::initializer_list<Field> fields = {});
    void debug(std::string_view event, std::initializer_list<Field> fields = {});
    void info(std::string_view event, std::initializer_list<Field> fields = {});
    void warn(std::string_view event, std::initializer_list<Field> fields = {});
    void error(std::string_view event, std::initializer_list<Field> fields = {});

    /// JSON string escaping helper (exposed for /statusz JSON rendering too).
    std::string jsonEscape(std::string_view s);
}
