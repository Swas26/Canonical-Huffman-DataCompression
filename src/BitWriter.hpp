#pragma once

#include <cstdint>
#include <vector>

class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& dest_buffer);
    ~BitWriter();

    BitWriter(const BitWriter&) = delete;
    BitWriter& operator=(const BitWriter&) = delete;

    void write_bit(bool bit);
    void write_bits(uint64_t value, int count);
    void flush();

private:
    std::vector<uint8_t>& buffer;
    uint8_t current_byte;
    int bit_count;
};
