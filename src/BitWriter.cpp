#include "BitWriter.hpp"

BitWriter::BitWriter(std::vector<uint8_t>& dest_buffer)
    : buffer(dest_buffer), current_byte(0), bit_count(0) {}

BitWriter::~BitWriter() {
    flush();
}

void BitWriter::write_bit(bool bit) {
    if (bit) {
        current_byte |= (1 << (7 - bit_count));
    }
    bit_count++;

    if (bit_count == 8) {
        buffer.push_back(current_byte);
        current_byte = 0;
        bit_count = 0;
    }
}

void BitWriter::write_bits(uint64_t value, int count) {
    for (int i = count - 1; i >= 0; --i) {
        bool bit = (value >> i) & 1;
        write_bit(bit);
    }
}

void BitWriter::flush() {
    if (bit_count > 0) {
        buffer.push_back(current_byte);
        current_byte = 0;
        bit_count = 0;
    }
}
