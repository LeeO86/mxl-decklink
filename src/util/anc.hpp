// SPDX-License-Identifier: MIT
// SMPTE 291 ancillary data serialization per RFC 8331 §2 for the MXL
// `video/smpte291` flow (SPECIFICATION.md §2.3).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mxldl::util
{
    /// One ancillary data packet as delivered by the DeckLink SDK
    /// (IDeckLinkAncillaryPacket, 8-bit UDW representation).
    struct AncPacket
    {
        std::uint8_t did = 0;
        std::uint8_t sdid = 0;
        std::uint32_t lineNumber = 0;
        std::uint8_t dataStreamIndex = 0; // SDK data stream: 0 luma (Y; SD), 1 chroma (C) → RFC 8331 C bit
        std::vector<std::uint8_t> userData; // UDW payload, 8-bit values
    };

    /// Serializes packets into the RFC 8331 §2 payload from the Length field
    /// onward, as MXL stores it (no RTP header, no extended sequence number):
    ///   Length(16) | ANC_Count(8) | F(2) | reserved(22) |   (F = `field`; 6 bytes)
    ///   per packet, from byte 6 on: C(1) LineNumber(11) HorizOffset(12) S(1) StreamNum(7)
    ///               DID(10) SDID(10) DataCount(10) UDW(10 × n) Checksum(10)
    ///               word_align (zero bits to a multiple of 32 bits per packet)
    /// DID/SDID/DC/UDW 10-bit words carry the SMPTE 291 parity bits (b8 =
    /// even parity of b0..7, b9 = !b8).
    /// \param maxBytes grain capacity; packets that would overflow are dropped
    ///        (count is reflected in the return structure).
    struct AncEncodeResult
    {
        std::vector<std::uint8_t> payload;
        std::size_t packetsEncoded = 0;
        std::size_t packetsDropped = 0;
    };

    /// RFC 8331 F field: progressive (or unspecified), or the field of an
    /// interlaced picture the packets belong to.
    constexpr std::uint8_t kAncFieldProgressive = 0b00;
    constexpr std::uint8_t kAncField1 = 0b10;
    constexpr std::uint8_t kAncField2 = 0b11;

    AncEncodeResult encodeRfc8331(std::vector<AncPacket> const& packets, std::size_t maxBytes, std::uint8_t field = kAncFieldProgressive);

    /// Adds the SMPTE 291 parity bits to an 8-bit word (returns the 10-bit word).
    std::uint16_t addParity(std::uint8_t value);

    /// Decoder for tests and the (future) output-side ANC insertion.
    std::vector<AncPacket> decodeRfc8331(std::uint8_t const* data, std::size_t size);
}
