#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// The mirror image of BitWriter: MSB-first, same packing, same clamping rules.
// A non-owning view -- the caller keeps the bytes alive for the reader's life.
//
// There is no pad-bit count in the stream (see BitWriter). The caller is
// expected to stop after the N symbols named in the header; the zero padding in
// the last byte is simply never read.
class BitReader {
public:
    BitReader(const uint8_t* data, std::size_t size);
    explicit BitReader(const std::vector<uint8_t>& source);

    BitReader(const BitReader&) = delete;
    BitReader& operator=(const BitReader&) = delete;

    bool read_bit();

    // Consumes `count` bits, most significant first, and returns them in the low
    // `count` bits of the result. `count` is clamped the same way BitWriter
    // clamps it: <= 0 reads nothing and returns 0, > 64 reads 64. Reading past
    // the end yields zeros for the missing bits and sets the overrun flag.
    uint64_t read_bits(int count);

    // Same value read_bits(count) would return, without consuming anything.
    //
    // Past the end of the stream the missing bits read as 0 and NO overrun is
    // flagged. That is deliberate: a table decoder peeks a fixed window the
    // width of the longest code, and near the end of the stream that window
    // legitimately overhangs the last code. Peeking is a look, not a read; only
    // the subsequent read_bits() of the decoded code length can overrun.
    uint64_t peek_bits(int count) const;

    // Advances without decoding. Clamped and flagged like read_bits().
    void skip_bits(int count);

    // Discards bits up to the next byte boundary; no-op when already aligned.
    // The counterpart of BitWriter::flush().
    void align_to_byte();

    uint64_t bits_read() const { return bit_pos; }
    uint64_t bits_remaining() const { return bit_pos < total_bits ? total_bits - bit_pos : 0; }
    bool exhausted() const { return bit_pos >= total_bits; }

    // Sticky: true once any read or skip has run past the end of the buffer.
    // A well-formed stream decoded against its header count never sets it.
    bool overrun() const { return overran; }
    bool ok() const { return !overran; }

private:
    const uint8_t* buffer;
    uint64_t total_bits;
    uint64_t bit_pos;
    bool overran;
};
