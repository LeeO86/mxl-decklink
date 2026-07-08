// SPDX-License-Identifier: MIT
#include "logging.hpp"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <stdexcept>

namespace mxldl::log
{
    namespace
    {
        std::atomic<Level> g_level{Level::Info};
        std::atomic<Format> g_format{Format::Json};
        std::mutex g_writeMutex;

        char const* levelName(Level l)
        {
            switch (l)
            {
                case Level::Trace: return "trace";
                case Level::Debug: return "debug";
                case Level::Info: return "info";
                case Level::Warn: return "warn";
                case Level::Error: return "error";
            }
            return "?";
        }

        std::string isoTimestamp()
        {
            timespec ts{};
            ::clock_gettime(CLOCK_REALTIME, &ts);
            tm tmv{};
            ::gmtime_r(&ts.tv_sec, &tmv);
            char buf[40];
            auto const n = ::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);
            char out[56];
            ::snprintf(out, sizeof(out), "%.*s.%03ldZ", static_cast<int>(n), buf, ts.tv_nsec / 1'000'000L);
            return out;
        }
    }

    std::string jsonEscape(std::string_view s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (char const c : s)
        {
            switch (c)
            {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char buf[8];
                        ::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    }
                    else
                    {
                        out += c;
                    }
            }
        }
        return out;
    }

    void configure(Level level, Format format)
    {
        g_level.store(level, std::memory_order_relaxed);
        g_format.store(format, std::memory_order_relaxed);
    }

    Level currentLevel()
    {
        return g_level.load(std::memory_order_relaxed);
    }

    Level parseLevel(std::string_view s)
    {
        if (s == "trace")
        {
            return Level::Trace;
        }
        if (s == "debug")
        {
            return Level::Debug;
        }
        if (s == "info")
        {
            return Level::Info;
        }
        if (s == "warn")
        {
            return Level::Warn;
        }
        if (s == "error")
        {
            return Level::Error;
        }
        throw std::invalid_argument("invalid LOG_LEVEL: " + std::string(s));
    }

    Format parseFormat(std::string_view s)
    {
        if (s == "json")
        {
            return Format::Json;
        }
        if (s == "text")
        {
            return Format::Text;
        }
        throw std::invalid_argument("invalid LOG_FORMAT: " + std::string(s));
    }

    void write(Level level, std::string_view event, std::initializer_list<Field> fields)
    {
        if (level < g_level.load(std::memory_order_relaxed))
        {
            return;
        }

        std::string line;
        line.reserve(256);
        if (g_format.load(std::memory_order_relaxed) == Format::Json)
        {
            line += "{\"ts\":\"";
            line += isoTimestamp();
            line += "\",\"level\":\"";
            line += levelName(level);
            line += "\",\"event\":\"";
            line += jsonEscape(event);
            line += '"';
            for (auto const& f : fields)
            {
                line += ",\"";
                line += jsonEscape(f.key);
                line += "\":";
                if (f.isRaw)
                {
                    line += f.value;
                }
                else
                {
                    line += '"';
                    line += jsonEscape(f.value);
                    line += '"';
                }
            }
            line += "}\n";
        }
        else
        {
            line += isoTimestamp();
            line += ' ';
            line += levelName(level);
            line += ' ';
            line += event;
            for (auto const& f : fields)
            {
                line += ' ';
                line += f.key;
                line += '=';
                line += f.value;
            }
            line += '\n';
        }

        std::lock_guard const lock{g_writeMutex};
        ::fwrite(line.data(), 1, line.size(), stderr);
        ::fflush(stderr);
    }

    void trace(std::string_view event, std::initializer_list<Field> fields)
    {
        write(Level::Trace, event, fields);
    }

    void debug(std::string_view event, std::initializer_list<Field> fields)
    {
        write(Level::Debug, event, fields);
    }

    void info(std::string_view event, std::initializer_list<Field> fields)
    {
        write(Level::Info, event, fields);
    }

    void warn(std::string_view event, std::initializer_list<Field> fields)
    {
        write(Level::Warn, event, fields);
    }

    void error(std::string_view event, std::initializer_list<Field> fields)
    {
        write(Level::Error, event, fields);
    }
}
