// SPDX-License-Identifier: MIT
// Persisted IS-05 activations (SPECIFICATION.md §7.7): the last /active of
// every MXL Sender and Receiver, restored after a restart so the outputs come
// back on their routes without a controller. No NMOS library types here, so
// unit tests and the default build use it without nmos-cpp.
#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "nmos/routing.hpp"

namespace mxldl::nmosroute
{
    /// One saved /active endpoint.
    struct SavedActivation
    {
        std::string legId;
        bool sender = false;
        bool masterEnable = false;
        std::string domainId; // resolved mxl_domain_id
        /// Directory of that domain when it was activated: a restored route
        /// waits there when the domain is not (yet) found by id.
        std::string domainPath;
        std::optional<std::string> flowId; // nullopt = null
        /// `sender_id` of a Receiver / `receiver_id` of a Sender, so /active
        /// names the other end again after a restart.
        std::optional<std::string> peerId;
    };

    /// `<config dir>/state/connections.json`: the directory of the
    /// configuration file (`MXL_CONFIG_FILE`, default `CONFIG_DIR`).
    [[nodiscard]] std::string routeStatePath(config::Config const& cfg);

    /// The saved activations, written atomically (temp file + rename) on
    /// every change. Not part of config export/import.
    class RouteState
    {
    public:
        explicit RouteState(std::string path);

        /// Reads the file. A missing file is empty; an unreadable or invalid
        /// one is ignored with a warning (no routes restored).
        void load();

        [[nodiscard]] std::vector<SavedActivation> all() const;
        [[nodiscard]] std::optional<SavedActivation> find(std::string const& legId) const;

        /// Stores one activation and rewrites the file. Returns false when the
        /// write failed (logged).
        bool save(SavedActivation const& activation);

        [[nodiscard]] std::string const& path() const
        {
            return _path;
        }

    private:
        bool writeLocked();

        std::string _path;
        mutable std::mutex _mutex;
        std::map<std::string, SavedActivation> _entries; // by leg id
    };

    struct RestoreOutcome
    {
        enum class Kind
        {
            Restored, // applied as saved
            Waiting, // applied; the domain or flow is not there yet, the receiver waits for it
            Disabled, // the saved route was rejected; the leg comes up with master_enable false
            Skipped, // no such Sender/Receiver any more
        };
        Kind kind = Kind::Restored;
        /// What /active shows after the restore (masterEnable false when
        /// Disabled).
        SavedActivation active;
        std::string reason; // Waiting, Disabled, Skipped
    };

    /// Applies the saved activations to `channels` before the channels start.
    /// Receivers keep their saved domain and flow; a domain that is not found
    /// by id is read at its saved directory once it exists, and a flow that
    /// is not there yet is waited for (the output channel retries). Senders
    /// restore master_enable only: their flow ids come from the configuration
    /// (an input's flow id is this function's own). A saved route that
    /// applyActivation rejects comes up disabled.
    [[nodiscard]] std::vector<RestoreOutcome> restoreActivations(std::vector<config::ChannelConfig>& channels, std::vector<Leg> const& legs,
        std::vector<SavedActivation> const& saved, std::vector<DomainBinding> const& domains, std::string const& primaryDomainId,
        std::string const& primaryDomainPath);

    [[nodiscard]] char const* restoreKindName(RestoreOutcome::Kind kind);
}
