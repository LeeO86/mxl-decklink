// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mxldl::util
{
    /// Canonical lowercase 8-4-4-4-12 UUID handling.
    struct Uuid
    {
        std::array<std::uint8_t, 16> bytes{};

        [[nodiscard]] std::string toString() const;
        [[nodiscard]] bool isNil() const;

        bool operator==(Uuid const&) const = default;
    };

    /// Parses a canonical UUID string; returns nullopt on malformed input.
    std::optional<Uuid> parseUuid(std::string_view s);

    /// Deterministic derivation of a replacement flow UUID after a format
    /// change (§3.8): SHA-1-free name-based mix of the configured UUID and a
    /// format signature (RFC 4122 version-4 layout, deterministic content).
    Uuid deriveUuid(Uuid const& base, std::string_view name);
}
