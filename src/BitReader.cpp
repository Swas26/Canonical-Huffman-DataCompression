#include "BitReader.hpp"

BitReader::BitReader(const uint8_t* data, std::size_t size)
    : buffer(data),
      total_bits(static_cast<uint64_t>(size) * 8),
      bit_pos(0),
      overran(false) {}

BitReader::BitReader(const std::vector<uint8_t>& source)
    : BitReader(source.data(), source.size()) {}

bool BitReader::read_bit() {
    return read_bits(1) != 0;
}

uint64_t BitReader::peek_bits(int count) const {
    if (count <= 0) return 0;
    if (count > 64) count = 64;

    uint64_t result = 0;
    uint64_t pos = bit_pos;
    for (int i = 0; i < count; ++i, ++pos) {
        uint64_t bit = 0;
        if (pos < total_bits) {
            // Same addressing as BitWriter::write_bit: bit 0 of the stream is
            // bit 7 of byte 0.
            bit = (buffer[pos >> 3] >> (7 - (pos & 7))) & 1;
        }
        result = (result << 1) | bit;
    }
    return result;
}

uint64_t BitReader::read_bits(int count) {
    if (count <= 0) return 0;
    if (count > 64) count = 64;

    uint64_t result = peek_bits(count);
    if (static_cast<uint64_t>(count) > bits_remaining()) overran = true;
    bit_pos += static_cast<uint64_t>(count);
    return result;
}

void BitReader::skip_bits(int count) {
    if (count <= 0) return;
    if (count > 64) count = 64;

    if (static_cast<uint64_t>(count) > bits_remaining()) overran = true;
    bit_pos += static_cast<uint64_t>(count);
}

void BitReader::align_to_byte() {
    // total_bits is always a multiple of 8, so this can never step past the
    // end of a stream the reader has not already overrun.
    bit_pos = (bit_pos + 7) & ~static_cast<uint64_t>(7);
}
