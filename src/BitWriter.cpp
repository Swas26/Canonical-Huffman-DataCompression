#include "BitWriter.hpp"

BitWriter::BitWriter(std::vector<uint8_t>& dest_buffer)
    : buffer(dest_buffer), bits_total(0), current_byte(0), bit_count(0) {}

BitWriter::~BitWriter() {
    flush();
}

void BitWriter::write_bit(bool bit) {
    if (bit) {
        current_byte |= static_cast<uint8_t>(1u << (7 - bit_count));
    }
    bit_count++;
    bits_total++;

    if (bit_count == 8) {
        buffer.push_back(current_byte);
        current_byte = 0;
        bit_count = 0;
    }
}

void BitWriter::write_bits(uint64_t value, int count) {
    if (count <= 0) return;
    if (count > 64) count = 64;

    for (int i = count - 1; i >= 0; --i) {
        write_bit(((value >> i) & 1) != 0);
    }
}

void BitWriter::flush() {
    if (bit_count > 0) {
        buffer.push_back(current_byte);
        current_byte = 0;
        bit_count = 0;
    }
}
