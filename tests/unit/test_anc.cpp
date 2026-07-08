// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "util/anc.hpp"

using namespace mxldl::util;

TEST_CASE("SMPTE 291 parity bits")
{
    // b8 = even parity of b0..7, b9 = !b8.
    CHECK(addParity(0x00) == 0x200); // 0 ones → b8=0, b9=1
    CHECK(addParity(0x01) == 0x101); // 1 one → b8=1, b9=0
    CHECK(addParity(0x03) == 0x203); // 2 ones → b8=0, b9=1
    CHECK(addParity(0x61) == 0x161); // 3 ones → b8=1
    CHECK(addParity(0xff) == 0x2ff); // 8 ones → b8=0
}

TEST_CASE("RFC 8331 encode/decode round-trip")
{
    std::vector<AncPacket> packets;
    AncPacket p1;
    p1.did = 0x61;
    p1.sdid = 0x02;
    p1.lineNumber = 9;
    p1.dataStreamIndex = 0;
    p1.userData = {0x01, 0x02, 0x03, 0x04, 0x05};
    packets.push_back(p1);

    AncPacket p2;
    p2.did = 0x41;
    p2.sdid = 0x07;
    p2.lineNumber = 571;
    p2.dataStreamIndex = 1;
    p2.userData = {}; // empty UDW is legal
    packets.push_back(p2);

    auto const encoded = encodeRfc8331(packets, 4096);
    CHECK(encoded.packetsEncoded == 2);
    CHECK(encoded.packetsDropped == 0);
    REQUIRE(encoded.payload.size() >= 8);
    // Header: Length(16) covers everything after the 8-byte header.
    std::size_t const length = (static_cast<std::size_t>(encoded.payload[0]) << 8) | encoded.payload[1];
    CHECK(length == encoded.payload.size() - 8);
    CHECK(encoded.payload[2] == 2); // ANC_Count
    // Data section is 32-bit aligned.
    CHECK(encoded.payload.size() % 4 == 0);

    auto const decoded = decodeRfc8331(encoded.payload.data(), encoded.payload.size());
    REQUIRE(decoded.size() == 2);
    CHECK(decoded[0].did == p1.did);
    CHECK(decoded[0].sdid == p1.sdid);
    CHECK(decoded[0].lineNumber == p1.lineNumber);
    CHECK(decoded[0].userData == p1.userData);
    CHECK(decoded[1].did == p2.did);
    CHECK(decoded[1].sdid == p2.sdid);
    CHECK(decoded[1].lineNumber == p2.lineNumber);
    CHECK(decoded[1].dataStreamIndex == p2.dataStreamIndex);
    CHECK(decoded[1].userData.empty());
}

TEST_CASE("empty packet list yields a valid empty payload")
{
    auto const encoded = encodeRfc8331({}, 4096);
    CHECK(encoded.packetsEncoded == 0);
    REQUIRE(encoded.payload.size() == 8);
    CHECK(encoded.payload[0] == 0);
    CHECK(encoded.payload[1] == 0);
    CHECK(encoded.payload[2] == 0);
}

TEST_CASE("oversized packets are dropped, not truncated")
{
    AncPacket big;
    big.did = 0x50;
    big.sdid = 0x01;
    big.userData.assign(255, 0xab);

    // Grain too small for the packet but large enough for the header.
    auto const encoded = encodeRfc8331({big}, 64);
    CHECK(encoded.packetsEncoded == 0);
    CHECK(encoded.packetsDropped == 1);
    CHECK(encoded.payload.size() == 8);

    // Large grain: packet fits.
    auto const encoded2 = encodeRfc8331({big}, 4096);
    CHECK(encoded2.packetsEncoded == 1);
    auto const decoded = decodeRfc8331(encoded2.payload.data(), encoded2.payload.size());
    REQUIRE(decoded.size() == 1);
    CHECK(decoded[0].userData.size() == 255);
}

TEST_CASE("checksum word follows ST 291-1 (9-bit sum, b9 = !b8)")
{
    AncPacket p;
    p.did = 0x61;
    p.sdid = 0x02;
    p.userData = {0x10};
    auto const e = encodeRfc8331({p}, 4096);

    // Decode manually to reach the checksum: skip header (8 bytes), then
    // C(1)+Line(11)+HO(12)+S(1)+Stream(7) = 32 bits, then DID/SDID/DC/UDW/CS.
    auto bits = [&](std::size_t bitPos, unsigned n) {
        std::uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i, ++bitPos)
        {
            v = (v << 1) | ((e.payload[8 + bitPos / 8] >> (7 - bitPos % 8)) & 1U);
        }
        return v;
    };
    std::uint32_t const did10 = bits(32, 10);
    std::uint32_t const sdid10 = bits(42, 10);
    std::uint32_t const dc10 = bits(52, 10);
    std::uint32_t const udw10 = bits(62, 10);
    std::uint32_t const cs10 = bits(72, 10);
    std::uint32_t const expected9 = (did10 + sdid10 + dc10 + udw10) & 0x1ff;
    CHECK((cs10 & 0x1ff) == expected9);
    CHECK(((cs10 >> 9) & 1U) == (((cs10 >> 8) & 1U) ^ 1U));
}
