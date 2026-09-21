#pragma once

#include <cstdint>
#include <vector>

// Bit order is MSB-first: the first bit written lands in bit 7 of the first
// byte, so a byte reads left to right in the order the bits were produced.
//
// Framing note: the container stores NO pad-bit count. The header carries the
// original symbol count instead, and the decoder stops after N symbols without
// ever looking at the zero padding in the last byte. The count has to be in the
// header anyway for the CRC check and the progress output, so it is free.
// BitReader is the mirror image of this class.
class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& dest_buffer);
    ~BitWriter();

    BitWriter(const BitWriter&) = delete;
    BitWriter& operator=(const BitWriter&) = delete;

    void write_bit(bool bit);

    // Writes the low `count` bits of `value`, most significant first. Bits above
    // `count` are ignored. `count` outside 1..64 is clamped: <= 0 writes nothing,
    // > 64 writes 64 (shifting a uint64_t by 64 or more is undefined).
    void write_bits(uint64_t value, int count);

    // Zero-pads the pending partial byte out to a byte boundary; no-op when
    // already aligned. The destructor calls this. BitReader::align_to_byte()
    // is the counterpart on the read side.
    void flush();

    // Bits handed to the writer so far, the pending partial byte included.
    // Padding added by flush() does not count. This is the figure the header's
    // symbol count and the progress output are derived from.
    uint64_t bits_written() const { return bits_total; }

    // Bits sitting in the partial byte, 0..7.
    int bits_pending() const { return bit_count; }

private:
    std::vector<uint8_t>& buffer;
    uint64_t bits_total;
    uint8_t current_byte;
    int bit_count;
};
