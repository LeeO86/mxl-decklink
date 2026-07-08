// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "util/uuid.hpp"

using namespace mxldl::util;

TEST_CASE("uuid parse/format round-trip")
{
    auto const u = parseUuid("5fbec3b1-1b0f-417d-9059-8b94a47197ed");
    REQUIRE(u.has_value());
    CHECK(u->toString() == "5fbec3b1-1b0f-417d-9059-8b94a47197ed");
    CHECK_FALSE(u->isNil());

    auto const upper = parseUuid("5FBEC3B1-1B0F-417D-9059-8B94A47197ED");
    REQUIRE(upper.has_value());
    CHECK(*upper == *u);
}

TEST_CASE("malformed uuids rejected")
{
    CHECK_FALSE(parseUuid("").has_value());
    CHECK_FALSE(parseUuid("5fbec3b1").has_value());
    CHECK_FALSE(parseUuid("5fbec3b1-1b0f-417d-9059-8b94a47197eg").has_value());
    CHECK_FALSE(parseUuid("5fbec3b11b0f-417d-9059-8b94a47197ed0").has_value());
}

TEST_CASE("derived uuids are deterministic, distinct and well-formed (§3.8)")
{
    auto const base = *parseUuid("5fbec3b1-1b0f-417d-9059-8b94a47197ed");
    auto const a = deriveUuid(base, "HD720p50#1");
    auto const b = deriveUuid(base, "HD720p50#1");
    auto const c = deriveUuid(base, "HD720p50#2");

    CHECK(a == b); // deterministic
    CHECK_FALSE(a == c); // signature-sensitive
    CHECK_FALSE(a == base);
    // RFC 4122 version 4 / variant 10 bits.
    CHECK((a.bytes[6] & 0xf0) == 0x40);
    CHECK((a.bytes[8] & 0xc0) == 0x80);
}
