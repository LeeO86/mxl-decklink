// SPDX-License-Identifier: MIT
#include "domain.hpp"

#include <filesystem>
#include <stdexcept>

#include "util/logging.hpp"

namespace mxldl::mxlbridge
{
    Domain::Domain(std::string domainPath)
        : _path(std::move(domainPath))
    {
        // Create the directory when missing so a first deploy can start with
        // only defaults (operators still should put it on tmpfs).
        if (!std::filesystem::is_directory(_path))
        {
            std::error_code ec;
            std::filesystem::create_directories(_path, ec);
            if (ec || !std::filesystem::is_directory(_path))
            {
                throw std::runtime_error("MXL domain path does not exist and could not be created: " + _path +
                                         (ec ? (" (" + ec.message() + ")") : ""));
            }
            log::info("mxl_domain_directory_created", {{"path", _path}});
        }

        bool isTmpfs = false;
        if (::mxlIsTmpFs(_path.c_str(), &isTmpfs) == MXL_STATUS_OK && !isTmpfs)
        {
            log::warn("mxl_domain_not_tmpfs",
                {
                    {"path", _path},
                    {"details", "MXL_DOMAIN_PATH should be a tmpfs mount for shared-memory performance"},
                });
        }

        _instance = ::mxlCreateInstance(_path.c_str(), nullptr);
        if (_instance == nullptr)
        {
            throw std::runtime_error("mxlCreateInstance failed for domain: " + _path);
        }
        log::info("mxl_instance_created", {{"domain", _path}});
    }

    Domain::~Domain()
    {
        if (_instance != nullptr)
        {
            ::mxlDestroyInstance(_instance);
        }
    }

    void Domain::garbageCollect()
    {
        if (_instance != nullptr)
        {
            ::mxlGarbageCollectFlows(_instance);
        }
    }
}
