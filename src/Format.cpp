#include "Format.hpp"

#include <algorithm>
#include <array>
#include <iterator>

/* header layout, multi byte ints are little endian (lowest byte first):
    [0... 3] magic "swas"
    [4] flags
    [5... 12] orig_len
    [13... 16] crc
    [17... 272] lengths[0] .. lengths[255]
*/

/* if a field in Header changes size, HEADER_SIZE has to change with it */
static_assert(sizeof(f::MAGIC) 
            + sizeof(f::Header::flags)
            + sizeof(f::Header::orig_len)
            + sizeof(f::Header::crc)
            + sizeof(f::Header::lengths)
            == f::HEADER_SIZE,
            "HEADER_SIZE does not match the fields of f::Header");



namespace {
    constexpr uint8_t KNOWN_FLAGS = f::FLAG_RAW;

    /* appends the low nbytes of value, lowest byte first */
    void putLE(std::vector<uint8_t>& out, uint64_t value, std::size_t nbytes){
        for (std::size_t i = 0; i < nbytes; ++i){
            out.push_back(static_cast<uint8_t>(value >> (8 * i)));
        }
    }

    /* mirror of putLE; caller makes sure p has nbytes left */
    uint64_t getLE(const uint8_t* p, std::size_t nbytes){
        uint64_t value = 0;
        for (std::size_t i = 0; i < nbytes; ++i){
            value |= static_cast<uint64_t>(p[i]) << (8 * i);
        }
        return value;
    }
}


void f::writeHeader(const Header& h, std::vector<uint8_t>& out){
    out.insert(out.end(), std::begin(MAGIC), std::end(MAGIC));
    out.push_back(h.flags);
    putLE(out, h.orig_len, sizeof(h.orig_len));
    putLE(out, h.crc, sizeof(h.crc));
    out.insert(out.end(), h.lengths.begin(), h.lengths.end());
}

bool f::readHeader(const uint8_t* data, std::size_t size, Header& h){
    /* the one length check: everything below reads inside these HEADER_SIZE bytes */
    if (data == nullptr || size < HEADER_SIZE) return false;
    if (!std::equal(std::begin(MAGIC), std::end(MAGIC), data)) return false;

    /* parse into a local so h is left untouched when the input is garbage */
    Header parsed;
    const uint8_t* p = data + sizeof(MAGIC);

    parsed.flags = *p++;
    if (parsed.flags & ~KNOWN_FLAGS) return false; /* a bit we dont know the meaning of */

    parsed.orig_len = getLE(p, sizeof(parsed.orig_len));
    p += sizeof(parsed.orig_len);

    parsed.crc = static_cast<uint32_t>(getLE(p, sizeof(parsed.crc)));
    p += sizeof(parsed.crc);

    std::copy(p, p + parsed.lengths.size(), parsed.lengths.begin());

    /* every length <= 15 and kraft sum fits, else no prefix code has these lengths */
    if (!hf::lengthsAreValid(parsed.lengths)) return false;

    const bool hasCodes = std::any_of(parsed.lengths.begin(), parsed.lengths.end(),
                                      [](uint8_t l) { return l != 0; });

    if (parsed.flags & FLAG_RAW){
        if (hasCodes) return false; /* raw payload has no code table */
    } else if (parsed.orig_len > 0 && !hasCodes){
        return false; /* bytes to decode but no codes to decode them with */
    }

    h = parsed;
    return true;
}

/* CRC-32 as used by zlib, png and gzip, so any of their tools can check our values */
namespace {
    /* the generator 0x04C11DB7 with its bits mirrored: we feed each byte in lowest bit first,
       so the division runs right to left and "shift left" becomes "shift right" */
    constexpr uint32_t CRC_POLY = 0xEDB88320u;

    /* table[b] := remainder after dividing byte b by the generator
       ie. the 8 bit steps of the long division for one byte, done once at compile time */
    constexpr std::array<uint32_t, 256> makeCrcTable(){
        std::array<uint32_t, 256> table{};
        for (uint32_t b = 0; b < 256; ++b){
            uint32_t r = b;
            for (int k = 0; k < 8; ++k){
                /* lead bit 1 => the generator goes into it, subtract (xor) it */
                r = (r & 1) ? (r >> 1) ^ CRC_POLY : (r >> 1);
            }
            table[b] = r;
        }
        return table;
    }

    constexpr std::array<uint32_t, 256> CRC_TABLE = makeCrcTable();
}

uint32_t f::crc32(const uint8_t* data, std::size_t size){
    /* starting at all 1s makes leading zero bytes change the crc; with 0 "\0\0abc" and "abc" would match */
    uint32_t crc = 0xFFFFFFFFu;

    if (data != nullptr){
        for (std::size_t i = 0; i < size; ++i){
            /* the byte lines up with the low 8 bits of the running remainder, one table lookup does all 8 steps */
            crc = (crc >> 8) ^ CRC_TABLE[(crc ^ data[i]) & 0xFF];
        }
    }

    /* final flip is part of the standard; empty input gives 0 */
    return crc ^ 0xFFFFFFFFu;
}

uint32_t f::crc32(const std::vector<uint8_t>& data){
    return crc32(data.data(), data.size());
}
