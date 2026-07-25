// SPDX-License-Identifier: MIT
#include "domainscan.hpp"

#include <filesystem>
#include <fstream>
#include <random>

#include <fcntl.h>
#include <picojson/picojson.h>
#include <sys/file.h>
#include <unistd.h>

#include <mxl/flowinfo.h>
#include <mxl/mxl.h>

#include "util/logging.hpp"
#include "util/uuid.hpp"

namespace fs = std::filesystem;

namespace mxldl::mxlbridge
{
    namespace
    {
        constexpr char const* kFlowSuffix = ".mxl-flow";
        constexpr char const* kDomainDefFile = "domain_def.json"; // mxl-hands-on convention
        constexpr char const* kOptionsFile = "options.json";
        constexpr char const* kHistoryOption = "urn:x-mxl:option:history_duration/v1.0";

        std::optional<picojson::object> readJsonObject(fs::path const& file)
        {
            std::ifstream in(file);
            if (!in)
            {
                return std::nullopt;
            }
            picojson::value root;
            if (!picojson::parse(root, in).empty() || !root.is<picojson::object>())
            {
                return std::nullopt;
            }
            return root.get<picojson::object>();
        }

        std::optional<std::string> getString(picojson::object const& obj, char const* key)
        {
            auto const it = obj.find(key);
            if (it == obj.end() || !it->second.is<std::string>())
            {
                return std::nullopt;
            }
            return it->second.get<std::string>();
        }

        bool isTmpfsPath(fs::path const& p)
        {
            bool tmpfs = false;
            return ::mxlIsTmpFs(p.c_str(), &tmpfs) == MXL_STATUS_OK && tmpfs;
        }

        std::size_t countFlows(fs::path const& dir)
        {
            std::size_t n = 0;
            std::error_code ec;
            for (auto const& e : fs::directory_iterator(dir, ec))
            {
                if (e.is_directory(ec) && e.path().extension() == kFlowSuffix)
                {
                    ++n;
                }
            }
            return n;
        }

        /// MXL liveness check: writers/readers hold a shared flock on the
        /// flow data file; if an exclusive non-blocking lock succeeds, the
        /// flow is inactive. (The same probe MXL's own garbage collector
        /// uses; the lock is released immediately.)
        bool flowActive(fs::path const& dataFile)
        {
            int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOATIME
            flags |= O_NOATIME;
#endif
            int fd = ::open(dataFile.c_str(), flags);
            if (fd < 0)
            {
                // O_NOATIME fails for files owned by other users; retry without.
                fd = ::open(dataFile.c_str(), O_RDONLY | O_CLOEXEC);
            }
            if (fd < 0)
            {
                return false;
            }
            bool const active = ::flock(fd, LOCK_EX | LOCK_NB) < 0;
            ::close(fd); // also releases the lock if it was taken
            return active;
        }

        /// Reads the public 2048-byte mxlFlowInfo header from the flow data
        /// file (mxl/flowinfo.h fixed layout, version 1).
        std::optional<mxlFlowInfo> readFlowInfo(fs::path const& dataFile)
        {
            int const fd = ::open(dataFile.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0)
            {
                return std::nullopt;
            }
            mxlFlowInfo info{};
            auto const n = ::pread(fd, &info, sizeof(info), 0);
            ::close(fd);
            if (n != static_cast<ssize_t>(sizeof(info)) || info.version != 1)
            {
                return std::nullopt;
            }
            return info;
        }

        util::Uuid randomUuid()
        {
            std::random_device rd;
            util::Uuid u{};
            for (auto& b : u.bytes)
            {
                b = static_cast<std::uint8_t>(rd() & 0xff);
            }
            u.bytes[6] = static_cast<std::uint8_t>((u.bytes[6] & 0x0f) | 0x40);
            u.bytes[8] = static_cast<std::uint8_t>((u.bytes[8] & 0x3f) | 0x80);
            return u;
        }

        std::optional<DomainInfo> inspectDomainDir(fs::path const& dir)
        {
            std::error_code ec;
            bool const hasDomainDef = fs::exists(dir / kDomainDefFile, ec);
            bool const hasOptions = fs::exists(dir / kOptionsFile, ec);
            std::size_t const flows = countFlows(dir);
            if (!hasDomainDef && !hasOptions && flows == 0)
            {
                return std::nullopt;
            }

            DomainInfo info;
            info.path = dir.string();
            info.isTmpfs = isTmpfsPath(dir);
            info.hasOptionsJson = hasOptions;
            info.flowCount = flows;

            if (hasDomainDef)
            {
                if (auto const def = readJsonObject(dir / kDomainDefFile))
                {
                    info.id = getString(*def, "id");
                    info.label = getString(*def, "label");
                    info.description = getString(*def, "description");
                }
            }
            if (hasOptions)
            {
                if (auto const opts = readJsonObject(dir / kOptionsFile))
                {
                    if (auto const it = opts->find(kHistoryOption); it != opts->end() && it->second.is<double>())
                    {
                        info.historyDurationNs = static_cast<std::uint64_t>(it->second.get<double>());
                    }
                }
            }
            return info;
        }

        void scanRecursive(fs::path const& dir, int depth, std::vector<DomainInfo>& out)
        {
            if (depth < 0)
            {
                return;
            }
            if (auto info = inspectDomainDir(dir))
            {
                out.push_back(std::move(*info));
                return; // domains do not nest
            }
            std::error_code ec;
            for (auto const& e : fs::directory_iterator(dir, ec))
            {
                if (e.is_directory(ec) && !e.is_symlink(ec))
                {
                    scanRecursive(e.path(), depth - 1, out);
                }
            }
        }
    }

    std::vector<DomainInfo> scanDomains(std::string const& root, int maxDepth)
    {
        std::vector<DomainInfo> out;
        std::error_code ec;
        if (!fs::is_directory(root, ec))
        {
            return out;
        }
        scanRecursive(fs::path(root), maxDepth, out);
        return out;
    }

    std::vector<FlowSummary> listFlows(std::string const& domainPath)
    {
        std::vector<FlowSummary> out;
        std::error_code ec;
        for (auto const& e : fs::directory_iterator(domainPath, ec))
        {
            if (!e.is_directory(ec) || e.path().extension() != kFlowSuffix)
            {
                continue;
            }
            FlowSummary flow;
            flow.id = e.path().stem().string();

            if (auto const def = readJsonObject(e.path() / "flow_def.json"))
            {
                flow.format = getString(*def, "format").value_or("");
                flow.mediaType = getString(*def, "media_type").value_or("");
                flow.label = getString(*def, "label").value_or("");
                if (auto const tags = def->find("tags"); tags != def->end() && tags->second.is<picojson::object>())
                {
                    auto const& tagsObj = tags->second.get<picojson::object>();
                    if (auto const gh = tagsObj.find("urn:x-nmos:tag:grouphint/v1.0");
                        gh != tagsObj.end() && gh->second.is<picojson::array>() && !gh->second.get<picojson::array>().empty() &&
                        gh->second.get<picojson::array>()[0].is<std::string>())
                    {
                        flow.groupHint = gh->second.get<picojson::array>()[0].get<std::string>();
                    }
                }
                auto getNum = [&](char const* key) -> std::optional<double> {
                    auto const it = def->find(key);
                    if (it == def->end() || !it->second.is<double>())
                    {
                        return std::nullopt;
                    }
                    return it->second.get<double>();
                };
                if (auto const w = getNum("frame_width"))
                {
                    flow.width = static_cast<std::uint32_t>(*w);
                }
                if (auto const h = getNum("frame_height"))
                {
                    flow.height = static_cast<std::uint32_t>(*h);
                }
                if (auto const c = getNum("channel_count"))
                {
                    flow.channelCount = static_cast<std::uint32_t>(*c);
                }
                if (auto const im = getString(*def, "interlace_mode"))
                {
                    flow.interlaced = *im != "progressive";
                }
                char const* rateKey = flow.format == "urn:x-nmos:format:audio" ? "sample_rate" : "grain_rate";
                if (auto const rate = def->find(rateKey); rate != def->end() && rate->second.is<picojson::object>())
                {
                    auto const& r = rate->second.get<picojson::object>();
                    if (auto const n = r.find("numerator"); n != r.end() && n->second.is<double>())
                    {
                        flow.rateNumerator = static_cast<std::int64_t>(n->second.get<double>());
                    }
                    if (auto const d = r.find("denominator"); d != r.end() && d->second.is<double>())
                    {
                        flow.rateDenominator = static_cast<std::int64_t>(d->second.get<double>());
                    }
                    else if (flow.rateNumerator)
                    {
                        flow.rateDenominator = 1;
                    }
                }
            }

            auto const dataFile = e.path() / "data";
            flow.active = flowActive(dataFile);
            if (auto const info = readFlowInfo(dataFile))
            {
                flow.headIndex = info->runtime.headIndex;
                flow.lastWriteTimeNs = info->runtime.lastWriteTime;
                if (mxlIsDiscreteDataFormat(static_cast<int>(info->config.common.format)))
                {
                    flow.grainCount = info->config.discrete.grainCount;
                }
            }
            out.push_back(std::move(flow));
        }
        return out;
    }

    std::variant<CreateDomainResult, std::string> createDomain(CreateDomainRequest const& request, std::string const& scanRoot)
    {
        std::error_code ec;

        // Containment check: the new domain must live under the scan root
        // (§7.6) — prevents the API from writing anywhere else.
        auto const rootCanon = fs::weakly_canonical(scanRoot, ec);
        if (ec || rootCanon.empty())
        {
            return "domain scan root does not exist: " + scanRoot;
        }
        auto const target = fs::weakly_canonical(fs::path(request.path), ec);
        if (ec || target.empty())
        {
            return "invalid domain path: " + request.path;
        }
        auto const rootStr = rootCanon.string();
        auto const targetStr = target.string();
        if (targetStr != rootStr && targetStr.rfind(rootStr + "/", 0) != 0)
        {
            return "domain path must be inside the scan root " + rootStr;
        }
        if (targetStr == rootStr)
        {
            return "domain path must be a subdirectory of the scan root, not the root itself";
        }

        if (fs::exists(target / kDomainDefFile, ec) || countFlows(target) > 0)
        {
            return "a domain already exists at " + targetStr;
        }

        if (!fs::create_directories(target, ec) && ec)
        {
            return "cannot create " + targetStr + ": " + ec.message();
        }

        auto const id = randomUuid();
        {
            std::ofstream out(target / kDomainDefFile, std::ios::trunc);
            if (!out)
            {
                return "cannot write " + (target / kDomainDefFile).string();
            }
            out << "{\n  \"id\": \"" << id.toString() << "\",\n  \"label\": \"" << log::jsonEscape(request.label) << "\",\n  \"description\": \""
                << log::jsonEscape(request.description) << "\"\n}\n";
        }
        if (request.historyDurationNs)
        {
            std::ofstream out(target / kOptionsFile, std::ios::trunc);
            if (!out)
            {
                return "cannot write " + (target / kOptionsFile).string();
            }
            out << "{\n  \"" << kHistoryOption << "\": " << *request.historyDurationNs << "\n}\n";
        }

        CreateDomainResult result;
        result.path = targetStr;
        result.id = id.toString();
        result.isTmpfs = isTmpfsPath(target);
        log::info("mxl_domain_created",
            {
                {"path", result.path},
                {"id", result.id},
                {"label", request.label},
                {"tmpfs", result.isTmpfs},
            });
        return result;
    }
}
