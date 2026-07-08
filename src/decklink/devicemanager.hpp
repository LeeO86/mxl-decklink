// SPDX-License-Identifier: MIT
// Device enumeration, card matching and channel-to-sub-device mapping
// (SPECIFICATION.md §3.1).
#pragma once

#include <memory>
#include <string>
#include <variant>

#include "config/config.hpp"
#include "decklink/device.hpp"

namespace mxldl::dl
{
    /// Opens the configured backend and matches the configured card. Also
    /// validates every channel's sub-device index against the card (§4.3).
    /// Returns the card or an error string (card-level startup failure, §3.10
    /// retry semantics apply at the call site).
    std::variant<std::unique_ptr<ICard>, std::string> openConfiguredCard(config::Config const& cfg, config::EnvReader const& env);
}
