// SPDX-License-Identifier: MIT
#include "nmos/route_state.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <unistd.h>

#include <picojson/picojson.h>

#include "util/logging.hpp"
#include "util/uuid.hpp"

namespace mxldl::nmosroute
{
    namespace
    {
        picojson::value optionalString(std::optional<std::string> const& s)
        {
            return s ? picojson::value(*s) : picojson::value();
        }

        std::optional<std::string> readOptionalString(picojson::object const& o, char const* key)
        {
            auto const it = o.find(key);
            if (it == o.end() || !it->second.is<std::string>())
            {
                return std::nullopt;
            }
            return it->second.get<std::string>();
        }

        DomainBinding const* findDomain(std::vector<DomainBinding> const& domains, std::string const& id)
        {
            for (auto const& d : domains)
            {
                if (d.id == id)
                {
                    return &d;
                }
            }
            return nullptr;
        }

        /// Writes `text` to `path` and flushes it to the disk. Empty on
        /// success, else the error.
        std::string writeDurably(std::string const& path, std::string const& text)
        {
            int const fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd < 0)
            {
                return "cannot open " + path + ": " + std::strerror(errno);
            }
            std::size_t done = 0;
            while (done < text.size())
            {
                auto const n = ::write(fd, text.data() + done, text.size() - done);
                if (n < 0 && errno == EINTR)
                {
                    continue;
                }
                if (n <= 0)
                {
                    auto const err = std::string("cannot write ") + path + ": " + std::strerror(errno);
                    ::close(fd);
                    return err;
                }
                done += static_cast<std::size_t>(n);
            }
            if (::fsync(fd) != 0)
            {
                auto const err = std::string("fsync ") + path + ": " + std::strerror(errno);
                ::close(fd);
                return err;
            }
            ::close(fd);
            return {};
        }

        /// The rename itself is durable only once its directory is synced.
        void syncDirectory(std::filesystem::path const& dir)
        {
            int const fd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (fd >= 0)
            {
                (void)::fsync(fd);
                ::close(fd);
            }
        }

        Leg const* findLeg(std::vector<Leg> const& legs, std::string const& id)
        {
            for (auto const& leg : legs)
            {
                if (leg.id == id)
                {
                    return &leg;
                }
            }
            return nullptr;
        }
    }

    std::string routeStatePath(config::Config const& cfg)
    {
        std::filesystem::path dir;
        if (cfg.configFile && !cfg.configFile->empty())
        {
            dir = std::filesystem::path(*cfg.configFile).parent_path();
        }
        if (dir.empty())
        {
            dir = cfg.configDir;
        }
        return (dir / "state" / "connections.json").string();
    }

    RouteState::RouteState(std::string path)
        : _path(std::move(path))
    {
    }

    void RouteState::load()
    {
        std::lock_guard const lock{_mutex};
        _entries.clear();
        std::ifstream in(_path);
        if (!in)
        {
            return; // first start
        }
        std::stringstream text;
        text << in.rdbuf();
        in.close();
        // A broken file is kept for a look, not overwritten by the next save
        // with one route.
        auto const setAside = [this](std::string const& details) {
            auto const bad = _path + ".bad";
            std::error_code ec;
            std::filesystem::rename(_path, bad, ec);
            log::warn("route_state_invalid",
                {
                    {"path", _path},
                    {"details", details},
                    {"moved_to", ec ? std::string("(not moved: ") + ec.message() + ")" : bad},
                });
        };
        picojson::value root;
        auto const err = text.str().empty() ? std::string("empty file") : picojson::parse(root, text.str());
        if (!err.empty() || !root.is<picojson::object>())
        {
            setAside(err.empty() ? std::string("not a JSON object") : err);
            return;
        }
        auto const& top = root.get<picojson::object>();
        auto const resources = top.find("resources");
        if (resources == top.end() || !resources->second.is<picojson::object>())
        {
            setAside("no resources object");
            return;
        }
        for (auto const& [id, entry] : resources->second.get<picojson::object>())
        {
            if (!util::parseUuid(id) || !entry.is<picojson::object>())
            {
                continue;
            }
            auto const& e = entry.get<picojson::object>();
            auto const active = e.find("active");
            if (active == e.end() || !active->second.is<picojson::object>())
            {
                continue;
            }
            auto const& a = active->second.get<picojson::object>();
            SavedActivation saved;
            saved.legId = id;
            saved.sender = readOptionalString(e, "type").value_or("receiver") == "sender";
            auto const enable = a.find("master_enable");
            saved.masterEnable = enable != a.end() && enable->second.is<bool>() && enable->second.get<bool>();
            saved.domainId = readOptionalString(a, "mxl_domain_id").value_or("");
            saved.domainPath = readOptionalString(a, "mxl_domain_path").value_or("");
            saved.flowId = readOptionalString(a, "mxl_flow_id");
            saved.peerId = readOptionalString(a, saved.sender ? "receiver_id" : "sender_id");
            _entries[id] = std::move(saved);
        }
        log::info("route_state_loaded", {{"path", _path}, {"resources", static_cast<std::uint64_t>(_entries.size())}});
    }

    std::vector<SavedActivation> RouteState::all() const
    {
        std::lock_guard const lock{_mutex};
        std::vector<SavedActivation> out;
        out.reserve(_entries.size());
        for (auto const& [id, saved] : _entries)
        {
            out.push_back(saved);
        }
        return out;
    }

    std::optional<SavedActivation> RouteState::find(std::string const& legId) const
    {
        std::lock_guard const lock{_mutex};
        auto const it = _entries.find(legId);
        if (it == _entries.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    bool RouteState::save(SavedActivation const& activation)
    {
        std::lock_guard const lock{_mutex};
        _entries[activation.legId] = activation;
        return writeLocked();
    }

    bool RouteState::writeLocked()
    {
        picojson::object resources;
        for (auto const& [id, saved] : _entries)
        {
            picojson::object active;
            active["master_enable"] = picojson::value(saved.masterEnable);
            active["mxl_domain_id"] = picojson::value(saved.domainId);
            active["mxl_domain_path"] = picojson::value(saved.domainPath);
            active["mxl_flow_id"] = optionalString(saved.flowId);
            active[saved.sender ? "receiver_id" : "sender_id"] = optionalString(saved.peerId);
            picojson::object entry;
            entry["type"] = picojson::value(std::string(saved.sender ? "sender" : "receiver"));
            entry["active"] = picojson::value(active);
            resources[id] = picojson::value(entry);
        }
        picojson::object top;
        top["version"] = picojson::value(1.0);
        top["resources"] = picojson::value(resources);
        auto const text = picojson::value(top).serialize(true);

        std::error_code ec;
        auto const dir = std::filesystem::path(_path).parent_path();
        if (!dir.empty())
        {
            std::filesystem::create_directories(dir, ec);
        }
        // Atomic and durable: write and fsync a temp file, rename it over the
        // file, fsync the directory.
        auto const tmp = _path + ".tmp";
        if (auto const failed = writeDurably(tmp, text); !failed.empty())
        {
            log::error("route_state_write_failed", {{"path", _path}, {"details", failed + " (is the config volume writable?)"}});
            return false;
        }
        std::filesystem::rename(tmp, _path, ec);
        if (ec)
        {
            log::error("route_state_write_failed", {{"path", _path}, {"details", ec.message()}});
            return false;
        }
        syncDirectory(dir);
        return true;
    }

    char const* restoreKindName(RestoreOutcome::Kind kind)
    {
        switch (kind)
        {
            case RestoreOutcome::Kind::Restored: return "restored";
            case RestoreOutcome::Kind::Waiting: return "waiting";
            case RestoreOutcome::Kind::Disabled: return "disabled";
            case RestoreOutcome::Kind::Skipped: return "skipped";
        }
        return "?";
    }

    std::vector<RestoreOutcome> restoreActivations(std::vector<config::ChannelConfig>& channels, std::vector<Leg> const& legs,
        std::vector<SavedActivation> const& saved, std::vector<DomainBinding> const& domains, std::string const& primaryDomainId,
        std::string const& primaryDomainPath)
    {
        std::vector<RestoreOutcome> outcomes;
        for (auto const& s : saved)
        {
            RestoreOutcome outcome;
            outcome.active = s;
            auto const* leg = findLeg(legs, s.legId);
            if (leg == nullptr || leg->sender != s.sender)
            {
                outcome.kind = RestoreOutcome::Kind::Skipped;
                outcome.reason = "no such " + std::string(s.sender ? "sender" : "receiver") + " in the current configuration";
                outcomes.push_back(std::move(outcome));
                continue;
            }

            Activation activation;
            activation.legId = s.legId;
            activation.masterEnable = s.masterEnable;
            auto bindings = domains;
            if (leg->sender)
            {
                // An input writes this function's own domain and flow ids.
                activation.domainId = primaryDomainId;
                outcome.active.domainId = primaryDomainId;
                outcome.active.domainPath = primaryDomainPath;
                outcome.active.flowId = leg->initialFlowId;
                for (auto const& ch : channels)
                {
                    if (ch.index != leg->channelIndex)
                    {
                        continue;
                    }
                    if (leg->kind == LegKind::Video)
                    {
                        outcome.active.flowId = ch.videoFlowId.toString();
                    }
                    else if (leg->kind == LegKind::Anc && ch.ancFlowId)
                    {
                        outcome.active.flowId = ch.ancFlowId->toString();
                    }
                    for (auto const& af : ch.audioFlows)
                    {
                        if (leg->kind == LegKind::Audio && af.index == leg->audioIndex)
                        {
                            outcome.active.flowId = af.flowId.toString();
                        }
                    }
                }
                activation.flowId = outcome.active.flowId;
            }
            else
            {
                activation.domainId = s.domainId;
                activation.flowId = s.flowId;
                auto const* domain = findDomain(domains, s.domainId);
                if (domain == nullptr && s.masterEnable)
                {
                    if (s.domainPath.empty())
                    {
                        outcome.reason = "mxl_domain_id " + s.domainId + " is not found and no directory was saved";
                    }
                    else
                    {
                        // The function that owns the domain is not up yet:
                        // read its saved directory once it exists.
                        bindings.push_back(DomainBinding{s.domainId, s.domainPath});
                        outcome.kind = RestoreOutcome::Kind::Waiting;
                        outcome.reason = "domain not found yet; the receiver waits for " + s.domainPath;
                    }
                }
                else if (domain != nullptr)
                {
                    outcome.active.domainPath = domain->path;
                    if (s.masterEnable && s.flowId && !std::filesystem::exists(std::filesystem::path(domain->path) / (*s.flowId + ".mxl-flow")))
                    {
                        outcome.kind = RestoreOutcome::Kind::Waiting;
                        outcome.reason = "flow not found yet; the receiver waits for it";
                    }
                }
            }

            std::optional<std::string> err;
            if (outcome.reason.empty() || outcome.kind == RestoreOutcome::Kind::Waiting)
            {
                err = applyActivation(channels, legs, activation, bindings, primaryDomainPath, false);
            }
            else
            {
                err = outcome.reason;
            }
            if (err)
            {
                // Same safety as a rejected PATCH: the leg comes up disabled,
                // keeping the saved flow id for the controller to see.
                outcome.kind = RestoreOutcome::Kind::Disabled;
                outcome.reason = *err;
                outcome.active.masterEnable = false;
                Activation off = activation;
                off.masterEnable = false;
                if (off.flowId && !util::parseUuid(*off.flowId))
                {
                    off.flowId.reset();
                    outcome.active.flowId.reset();
                }
                (void)applyActivation(channels, legs, off, bindings, primaryDomainPath, false);
            }
            outcomes.push_back(std::move(outcome));
        }
        return outcomes;
    }
}
