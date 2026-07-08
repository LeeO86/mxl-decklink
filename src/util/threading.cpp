// SPDX-License-Identifier: MIT
#include "threading.hpp"

#include <cerrno>
#include <cstring>

#include <pthread.h>
#include <sched.h>

namespace mxldl::util
{
    std::optional<std::vector<int>> parseCpuList(std::string_view s)
    {
        std::vector<int> cpus;
        std::size_t pos = 0;
        while (pos < s.size())
        {
            std::size_t end = s.find(',', pos);
            if (end == std::string_view::npos)
            {
                end = s.size();
            }
            std::string_view const token = s.substr(pos, end - pos);
            if (token.empty())
            {
                return std::nullopt;
            }
            std::size_t const dash = token.find('-');
            try
            {
                if (dash == std::string_view::npos)
                {
                    std::size_t consumed = 0;
                    int const cpu = std::stoi(std::string(token), &consumed);
                    if (consumed != token.size() || cpu < 0)
                    {
                        return std::nullopt;
                    }
                    cpus.push_back(cpu);
                }
                else
                {
                    std::size_t c1 = 0;
                    std::size_t c2 = 0;
                    std::string const lo{token.substr(0, dash)};
                    std::string const hi{token.substr(dash + 1)};
                    int const from = std::stoi(lo, &c1);
                    int const to = std::stoi(hi, &c2);
                    if (c1 != lo.size() || c2 != hi.size() || from < 0 || to < from)
                    {
                        return std::nullopt;
                    }
                    for (int cpu = from; cpu <= to; ++cpu)
                    {
                        cpus.push_back(cpu);
                    }
                }
            }
            catch (...)
            {
                return std::nullopt;
            }
            pos = end + 1;
        }
        if (cpus.empty())
        {
            return std::nullopt;
        }
        return cpus;
    }

    std::optional<std::string> setRealtimePriority(int priority)
    {
        sched_param param{};
        param.sched_priority = priority;
        if (::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &param) != 0)
        {
            return std::string("pthread_setschedparam failed: ") + std::strerror(errno);
        }
        return std::nullopt;
    }

    std::optional<std::string> pinToCpus(std::vector<int> const& cpus)
    {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int const cpu : cpus)
        {
            CPU_SET(cpu, &set);
        }
        if (::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) != 0)
        {
            return std::string("pthread_setaffinity_np failed: ") + std::strerror(errno);
        }
        return std::nullopt;
    }

    void setThreadName(std::string_view name)
    {
        char buf[16]{};
        std::size_t const n = name.size() < 15 ? name.size() : 15;
        std::memcpy(buf, name.data(), n);
        ::pthread_setname_np(::pthread_self(), buf);
    }
}
