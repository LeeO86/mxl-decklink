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
        enum class Missing
        {
            Create, // this function's own output domain
            Fail, // another function's domain, opened to read: never created here
        };

        /// Opens the MXL domain; throws std::runtime_error when the directory
        /// is missing (and `missing` is Fail or creating it failed) or the
        /// instance cannot be created. Logs a warning when the path is not
        /// tmpfs-backed (§4.1 requires tmpfs).
        explicit Domain(std::string domainPath, Missing missing = Missing::Create);
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
