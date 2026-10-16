// SPDX-License-Identifier: MIT
#include "anc.hpp"

namespace mxldl::util
{
    namespace
    {
        /// MSB-first bit writer over a byte vector.
        class BitWriter
        {
        public:
            explicit BitWriter(std::vector<std::uint8_t>& out)
                : _out(out)
            {}

            void put(std::uint32_t value, unsigned bits)
            {
                for (unsigned i = bits; i-- > 0;)
                {
                    putBit((value >> i) & 1U);
                }
            }

            /// Pads with zero bits to the next 32-bit boundary counted from bit `origin`.
            void alignTo32(std::size_t origin)
            {
                while ((_bitPos - origin) % 32 != 0)
                {
                    putBit(0);
                }
            }

            [[nodiscard]] std::size_t bitPos() const
            {
                return _bitPos;
            }

        private:
            void putBit(std::uint32_t bit)
            {
                if (_bitPos % 8 == 0)
                {
                    _out.push_back(0);
                }
                if (bit != 0)
                {
                    _out.back() |= static_cast<std::uint8_t>(0x80U >> (_bitPos % 8));
                }
                ++_bitPos;
            }

            std::vector<std::uint8_t>& _out;
            std::size_t _bitPos = 0;
        };

        class BitReader
        {
        public:
            BitReader(std::uint8_t const* data, std::size_t size)
                : _data(data)
                , _bits(size * 8)
            {}

            std::uint32_t get(unsigned bits)
            {
                std::uint32_t v = 0;
                for (unsigned i = 0; i < bits; ++i)
                {
                    v <<= 1;
                    if (_bitPos < _bits)
                    {
                        v |= static_cast<std::uint32_t>((_data[_bitPos / 8] >> (7 - _bitPos % 8)) & 1U);
                        ++_bitPos;
                    }
                }
                return v;
            }

            /// Skips to the next 32-bit boundary counted from bit `origin`.
            void alignTo32(std::size_t origin)
            {
                _bitPos = origin + (_bitPos - origin + 31) / 32 * 32;
            }

            [[nodiscard]] std::size_t bitPos() const
            {
                return _bitPos;
            }

            [[nodiscard]] bool exhausted() const
            {
                return _bitPos >= _bits;
            }

        private:
            std::uint8_t const* _data;
            std::size_t _bits;
            std::size_t _bitPos = 0;
        };

        /// RFC 8331 header from the Length field on: Length(16) ANC_Count(8) F(2)
        /// reserved(22). MXL stores the payload from Length on (docs/Architecture.md),
        /// so the packets start at byte 6.
        constexpr std::size_t kHeaderBytes = 6;

        /// Bits needed for one encoded packet, before 32-bit alignment.
        std::size_t packetBits(AncPacket const& p)
        {
            return 1 + 11 + 12 + 1 + 7 + 10 + 10 + 10 + 10 * p.userData.size() + 10;
        }
    }

    std::uint16_t addParity(std::uint8_t value)
    {
        unsigned ones = 0;
        for (unsigned i = 0; i < 8; ++i)
        {
            ones += (value >> i) & 1U;
        }
        std::uint16_t const b8 = static_cast<std::uint16_t>(ones & 1U);
        std::uint16_t const b9 = static_cast<std::uint16_t>(b8 ^ 1U);
        return static_cast<std::uint16_t>((b9 << 9) | (b8 << 8) | value);
    }

    AncEncodeResult encodeRfc8331(std::vector<AncPacket> const& packets, std::size_t maxBytes, std::uint8_t field)
    {
        AncEncodeResult result;
        if (maxBytes < kHeaderBytes)
        {
            result.packetsDropped = packets.size();
            return result;
        }

        // Select the packets that fit; the header is 6 bytes (Length + ANC_Count +
        // F + reserved), each packet is padded to a multiple of 32 bits.
        std::size_t availableBits = (maxBytes - kHeaderBytes) * 8;
        std::vector<AncPacket const*> selected;
        for (auto const& p : packets)
        {
            if (selected.size() >= 255)
            {
                ++result.packetsDropped;
                continue;
            }
            std::size_t const bits = (packetBits(p) + 31) / 32 * 32;
            if (bits > availableBits)
            {
                ++result.packetsDropped;
                continue;
            }
            availableBits -= bits;
            selected.push_back(&p);
        }

        result.payload.reserve(maxBytes);
        BitWriter w{result.payload};

        // Length is back-patched once the data section size is known.
        w.put(0, 16);
        w.put(static_cast<std::uint32_t>(selected.size()), 8);
        w.put(field & 0x3U, 2); // F: 00 progressive / unspecified, 10 field 1, 11 field 2
        w.put(0, 22); // reserved

        for (auto const* p : selected)
        {
            std::size_t const origin = w.bitPos();
            // C: 1 = colour-difference (chroma) data stream. The SDK's data stream
            // index is 0 for luma (Y, and the only stream in SD) and 1 for chroma
            // (HD, when the luma stream is full); anything else counts as luma.
            w.put(p->dataStreamIndex == 1 ? 1U : 0U, 1);
            w.put(p->lineNumber & 0x7ffU, 11);
            w.put(0, 12); // horizontal offset: unknown from the SDK
            // S = 0, StreamNum = 0: no multi-stream link number (ST 425-3 etc.) is known;
            // the Y/C stream is carried by C, not by StreamNum.
            w.put(0, 1);
            w.put(0, 7);

            std::uint16_t const did10 = addParity(p->did);
            std::uint16_t const sdid10 = addParity(p->sdid);
            std::uint16_t const dc10 = addParity(static_cast<std::uint8_t>(p->userData.size()));
            w.put(did10, 10);
            w.put(sdid10, 10);
            w.put(dc10, 10);

            // Checksum: 9-bit sum of DID..UDW 10-bit words, b9 = !b8 (ST 291-1).
            std::uint32_t sum = did10 + sdid10 + dc10;
            for (auto const b : p->userData)
            {
                std::uint16_t const w10 = addParity(b);
                w.put(w10, 10);
                sum += w10;
            }
            std::uint16_t const cs9 = static_cast<std::uint16_t>(sum & 0x1ffU);
            std::uint16_t const cs10 = static_cast<std::uint16_t>(((~cs9 & 0x100U) << 1) | cs9);
            w.put(cs10, 10);
            w.alignTo32(origin); // word_align: the packet is a multiple of 32 bits
        }

        // Back-patch Length: size in bytes of the data section (everything
        // after the 6-byte header), per RFC 8331 §2.1.
        std::size_t const totalBytes = result.payload.size();
        std::size_t const dataBytes = totalBytes - kHeaderBytes;
        result.payload[0] = static_cast<std::uint8_t>(dataBytes >> 8);
        result.payload[1] = static_cast<std::uint8_t>(dataBytes & 0xff);

        result.packetsEncoded = selected.size();
        return result;
    }

    std::vector<AncPacket> decodeRfc8331(std::uint8_t const* data, std::size_t size)
    {
        std::vector<AncPacket> out;
        if (size < kHeaderBytes)
        {
            return out;
        }
        BitReader r{data, size};
        r.get(16); // length
        std::uint32_t const count = r.get(8);
        r.get(2); // F
        r.get(22); // reserved

        for (std::uint32_t i = 0; i < count && !r.exhausted(); ++i)
        {
            std::size_t const origin = r.bitPos();
            AncPacket p;
            p.dataStreamIndex = r.get(1) != 0 ? 1 : 0; // C: chroma stream = index 1
            p.lineNumber = r.get(11);
            r.get(12); // horizontal offset
            r.get(1); // S
            r.get(7); // StreamNum
            p.did = static_cast<std::uint8_t>(r.get(10) & 0xff);
            p.sdid = static_cast<std::uint8_t>(r.get(10) & 0xff);
            std::uint32_t const dc = r.get(10) & 0xff;
            p.userData.reserve(dc);
            for (std::uint32_t k = 0; k < dc; ++k)
            {
                p.userData.push_back(static_cast<std::uint8_t>(r.get(10) & 0xff));
            }
            r.get(10); // checksum
            r.alignTo32(origin);
            out.push_back(std::move(p));
        }
        return out;
    }
}
