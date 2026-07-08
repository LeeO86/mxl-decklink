// SPDX-License-Identifier: MIT
// Shared MXL instance per domain (SPECIFICATION.md §2.2: one mxlInstance per
// process/domain, shared by all channels).
#pragma once

#include <memory>
#include <string>

#include <mxl/mxl.h>

namespace mxldl::mxlbridge
{
    class Domain
    {
    public:
        /// Opens the MXL domain; throws std::runtime_error when the directory
        /// is missing or the instance cannot be created. Logs a warning when
        /// the path is not tmpfs-backed (§4.1 requires tmpfs).
        explicit Domain(std::string domainPath);
        ~Domain();

        Domain(Domain const&) = delete;
        Domain& operator=(Domain const&) = delete;

        [[nodiscard]] mxlInstance instance() const
        {
            return _instance;
        }

        [[nodiscard]] std::string const& path() const
        {
            return _path;
        }

        /// §2.5: periodic housekeeping duty.
        void garbageCollect();

    private:
        std::string _path;
        mxlInstance _instance = nullptr;
    };
}
