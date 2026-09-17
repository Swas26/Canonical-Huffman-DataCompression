// test_bitreader.cpp -- BitReader on its own, fed literal bytes.
// BitWriter is deliberately not involved, so a failure here points at the reader.
#include "BitReader.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

using Bytes = std::vector<uint8_t>;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
TEST(BitReaderTest, EmptyVectorIsExhaustedFromTheStart) {
    Bytes buf;
    BitReader r(buf);
    EXPECT_TRUE(r.exhausted());
    EXPECT_TRUE(r.ok());
    EXPECT_FALSE(r.overrun());
    EXPECT_EQ(r.bits_read(), 0u);
    EXPECT_EQ(r.bits_remaining(), 0u);
}

TEST(BitReaderTest, NullPointerWithZeroSizeIsSafe) {
    BitReader r(nullptr, 0);
    EXPECT_TRUE(r.exhausted());
    EXPECT_EQ(r.peek_bits(64), 0u);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.read_bits(8), 0u);
    EXPECT_TRUE(r.overrun());
}

TEST(BitReaderTest, FreshReaderOverNonEmptyBuffer) {
    Bytes buf{0x01, 0x02, 0x03};
    BitReader r(buf);
    EXPECT_FALSE(r.exhausted());
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.bits_read(), 0u);
    EXPECT_EQ(r.bits_remaining(), 24u);
}

TEST(BitReaderTest, PointerAndVectorConstructorsAgree) {
    Bytes buf{0x12, 0x34, 0x56, 0x78, 0x9A};
    BitReader from_vec(buf);
    BitReader from_ptr(buf.data(), buf.size());

    EXPECT_EQ(from_vec.bits_remaining(), from_ptr.bits_remaining());
    for (int width : {3, 5, 1, 7, 11, 13}) {
        EXPECT_EQ(from_vec.read_bits(width), from_ptr.read_bits(width)) << "width " << width;
        EXPECT_EQ(from_vec.bits_read(), from_ptr.bits_read());
    }
    EXPECT_EQ(from_vec.ok(), from_ptr.ok());
}

TEST(BitReaderTest, ReadsOnlyTheFirstSizeBytesOfALargerArray) {
    const uint8_t arr[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    BitReader r(arr, 2);
    EXPECT_EQ(r.bits_remaining(), 16u);
    EXPECT_EQ(r.read_bits(16), 0xAABBu);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(r.exhausted());

    // arr[2] must not leak in: past the end reads as zero.
    EXPECT_EQ(r.peek_bits(8), 0u);
    EXPECT_EQ(r.read_bits(8), 0u);
    EXPECT_TRUE(r.overrun());
}

TEST(BitReaderTest, IsANonOwningView) {
    Bytes buf{0x00};
    BitReader r(buf);
    buf[0] = 0xFF;   // same storage, no reallocation
    EXPECT_EQ(r.read_bits(8), 0xFFu);
}

TEST(BitReaderTest, IsNotCopyable) {
    EXPECT_FALSE(std::is_copy_constructible<BitReader>::value);
    EXPECT_FALSE(std::is_copy_assignable<BitReader>::value);
}

// ---------------------------------------------------------------------------
// read_bit / read_bits
// ---------------------------------------------------------------------------
TEST(BitReaderTest, ReadBitIsMsbFirst) {
    Bytes buf{0xB2};   // 1011 0010
    BitReader r(buf);
    for (bool want : {true, false, true, true, false, false, true, false}) {
        EXPECT_EQ(r.read_bit(), want) << "at bit " << r.bits_read();
    }
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(r.exhausted());
}

TEST(BitReaderTest, EverySingleBitPositionIsAddressedCorrectly) {
    for (int p = 0; p < 16; ++p) {
        Bytes buf{0x00, 0x00};
        buf[p / 8] = static_cast<uint8_t>(0x80 >> (p % 8));
        BitReader r(buf);
        for (int i = 0; i < 16; ++i) {
            EXPECT_EQ(r.read_bit(), i == p) << "set bit " << p << ", read bit " << i;
        }
    }
}

TEST(BitReaderTest, EveryByteValueReadsBackWholeAndBitByBit) {
    Bytes buf(256);
    for (int v = 0; v < 256; ++v) buf[v] = static_cast<uint8_t>(v);

    BitReader whole(buf);
    BitReader bits(buf);
    for (int v = 0; v < 256; ++v) {
        EXPECT_EQ(whole.read_bits(8), static_cast<uint64_t>(v));
        uint64_t rebuilt = 0;
        for (int i = 0; i < 8; ++i) rebuilt = (rebuilt << 1) | (bits.read_bit() ? 1u : 0u);
        EXPECT_EQ(rebuilt, static_cast<uint64_t>(v));
    }
    EXPECT_TRUE(whole.ok());
    EXPECT_TRUE(bits.ok());
}

TEST(BitReaderTest, ReadBitsAcrossByteBoundaries) {
    Bytes buf{0x12, 0x34};   // 0001 0010 0011 0100
    BitReader r(buf);
    EXPECT_EQ(r.read_bits(4), 0x1u);
    EXPECT_EQ(r.read_bits(8), 0x23u);
    EXPECT_EQ(r.read_bits(4), 0x4u);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(r.exhausted());
}

TEST(BitReaderTest, ReadBitsOddWidths) {
    Bytes buf{0xB9, 0x9B};   // 101 | 1100110 | 011011
    BitReader r(buf);
    EXPECT_EQ(r.read_bits(3), 0b101u);
    EXPECT_EQ(r.read_bits(7), 0b1100110u);
    EXPECT_EQ(r.read_bits(6), 0b011011u);
    EXPECT_TRUE(r.ok());
}

TEST(BitReaderTest, ReadSixtyFourBitsIsBigEndian) {
    Bytes buf{0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};
    BitReader r(buf);
    EXPECT_EQ(r.read_bits(64), 0xDEADBEEFCAFEBABEull);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(r.exhausted());
}

TEST(BitReaderTest, ReadSixtyFourBitsFromAnOffset) {
    Bytes buf{0xF1, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x0F};
    BitReader r(buf);
    EXPECT_EQ(r.read_bits(4), 0xFu);
    EXPECT_EQ(r.read_bits(64), 0x123456789ABCDEF0ull);
    EXPECT_EQ(r.read_bits(4), 0xFu);
    EXPECT_TRUE(r.ok());
}

TEST(BitReaderTest, ReadBitsCountZeroReturnsZeroAndConsumesNothing) {
    Bytes buf{0xFF};
    BitReader r(buf);
    EXPECT_EQ(r.read_bits(0), 0u);
    EXPECT_EQ(r.bits_read(), 0u);
    EXPECT_TRUE(r.ok());
}

TEST(BitReaderTest, ReadBitsNegativeCountReturnsZeroAndConsumesNothing) {
    Bytes buf{0xFF};
    BitReader r(buf);
    EXPECT_EQ(r.read_bits(-3), 0u);
    EXPECT_EQ(r.read_bits(-1000), 0u);
    EXPECT_EQ(r.bits_read(), 0u);
    EXPECT_TRUE(r.ok());
}

TEST(BitReaderTest, ReadBitsCountZeroOnEmptyStreamDoesNotOverrun) {
    BitReader r(nullptr, 0);
    EXPECT_EQ(r.read_bits(0), 0u);
    EXPECT_TRUE(r.ok());
}

TEST(BitReaderTest, ReadBitsCountAboveSixtyFourIsClamped) {
    Bytes buf{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    BitReader r(buf);
    EXPECT_EQ(r.read_bits(100), 0x0102030405060708ull);
    EXPECT_EQ(r.bits_read(), 64u);
    EXPECT_EQ(r.bits_remaining(), 8u);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.read_bits(8), 0x09u);
}

// ---------------------------------------------------------------------------
// peek_bits
// ---------------------------------------------------------------------------
TEST(BitReaderTest, PeekMatchesTheFollowingRead) {
    Bytes buf{0xDE, 0xAD, 0xBE, 0xEF, 0x01};
    BitReader r(buf);
    for (int width : {3, 5, 7, 9, 8, 1, 2, 5}) {
        uint64_t peeked = r.peek_bits(width);
        EXPECT_EQ(r.read_bits(width), peeked) << "width " << width;
    }
    EXPECT_TRUE(r.ok());
}

TEST(BitReaderTest, PeekDoesNotAdvance) {
    Bytes buf{0xAB, 0xCD};
    BitReader r(buf);
    r.read_bits(3);
    r.peek_bits(1);
    r.peek_bits(13);
    r.peek_bits(64);
    EXPECT_EQ(r.bits_read(), 3u);
    EXPECT_EQ(r.bits_remaining(), 13u);
}

TEST(BitReaderTest, PeekIsRepeatable) {
    Bytes buf{0x5A, 0xC3};
    BitReader r(buf);
    r.read_bits(5);
    const uint64_t first = r.peek_bits(64);
    for (int i = 0; i < 5; ++i) EXPECT_EQ(r.peek_bits(64), first);
    EXPECT_TRUE(r.ok());
}

TEST(BitReaderTest, PeekPastEndZeroFillsWithoutFlaggingOverrun) {
    Bytes buf{0x80};
    BitReader r(buf);
    EXPECT_EQ(r.peek_bits(64), 1ull << 63);
    EXPECT_TRUE(r.ok());
    EXPECT_FALSE(r.overrun());
}

TEST(BitReaderTest, PeekWindowOverhangingTheEndHoldsTheTailLeftAligned) {
    // How a table decoder looks at the stream: a fixed 15-bit window that
    // overhangs the last few bits.
    Bytes buf{0xFF, 0xA0};   // last byte 1010 0000
    BitReader r(buf);
    r.read_bits(10);         // 6 bits left: 10 0000
    EXPECT_EQ(r.peek_bits(15), 0b100000000000000u);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.bits_read(), 10u);
}

TEST(BitReaderTest, PeekOnExhaustedStreamReturnsZeroWithoutOverrun) {
    Bytes buf{0xFF};
    BitReader r(buf);
    r.read_bits(8);
    EXPECT_EQ(r.peek_bits(15), 0u);
    EXPECT_TRUE(r.ok());
}

TEST(BitReaderTest, PeekCountZeroOrNegativeReturnsZero) {
    Bytes buf{0xFF};
    BitReader r(buf);
    EXPECT_EQ(r.peek_bits(0), 0u);
    EXPECT_EQ(r.peek_bits(-7), 0u);
    EXPECT_TRUE(r.ok());
}

TEST(BitReaderTest, PeekCountAboveSixtyFourIsClamped) {
    Bytes buf{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    BitReader r(buf);
    EXPECT_EQ(r.peek_bits(100), r.peek_bits(64));
    EXPECT_EQ(r.peek_bits(65), 0x0102030405060708ull);
}

// ---------------------------------------------------------------------------
// Overrun
// ---------------------------------------------------------------------------
TEST(BitReaderTest, ReadingExactlyToTheEndIsNotAnOverrun) {
    Bytes buf{0xAB, 0xCD};
    BitReader r(buf);
    EXPECT_EQ(r.read_bits(16), 0xABCDu);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(r.exhausted());
    EXPECT_EQ(r.bits_remaining(), 0u);
}

TEST(BitReaderTest, OneBitPastTheEndSetsOverrun) {
    Bytes buf{0xAB, 0xCD};
    BitReader r(buf);
    r.read_bits(16);
    EXPECT_FALSE(r.read_bit());
    EXPECT_TRUE(r.overrun());
    EXPECT_FALSE(r.ok());
}

TEST(BitReaderTest, OverlongReadZeroFillsTheMissingBits) {
    Bytes buf{0xFF};
    BitReader r(buf);
    EXPECT_EQ(r.read_bits(12), 0xFF0u);
    EXPECT_TRUE(r.overrun());
}

TEST(BitReaderTest, OverlongReadStraddlingTheEnd) {
    Bytes buf{0xB0};   // 1011 0000
    BitReader r(buf);
    r.read_bits(2);    // 6 bits left: 11 0000
    EXPECT_EQ(r.read_bits(16), 0b1100000000000000u);
    EXPECT_TRUE(r.overrun());
}

TEST(BitReaderTest, ReadingAnEmptyStreamSetsOverrun) {
    Bytes buf;
    BitReader r(buf);
    EXPECT_EQ(r.read_bits(1), 0u);
    EXPECT_TRUE(r.overrun());
}

TEST(BitReaderTest, AfterOverrunCountersSaturate) {
    Bytes buf{0xFF};
    BitReader r(buf);
    r.read_bits(12);
    EXPECT_EQ(r.bits_remaining(), 0u);
    EXPECT_TRUE(r.exhausted());
    EXPECT_GE(r.bits_read(), 8u);
}

TEST(BitReaderTest, OverrunIsSticky) {
    Bytes buf{0xFF};
    BitReader r(buf);
    r.read_bits(9);
    ASSERT_TRUE(r.overrun());

    r.align_to_byte();
    EXPECT_TRUE(r.overrun());
    r.read_bits(0);
    EXPECT_TRUE(r.overrun());
    r.peek_bits(8);
    EXPECT_TRUE(r.overrun());
    r.skip_bits(0);
    EXPECT_TRUE(r.overrun());
    EXPECT_EQ(r.read_bits(4), 0u);
    EXPECT_FALSE(r.ok());
}

// ---------------------------------------------------------------------------
// skip_bits
// ---------------------------------------------------------------------------
TEST(BitReaderTest, SkipAdvancesWithoutDecoding) {
    Bytes buf{0xAB, 0xCD};
    BitReader r(buf);
    r.skip_bits(4);
    EXPECT_EQ(r.bits_read(), 4u);
    EXPECT_EQ(r.read_bits(8), 0xBCu);
    EXPECT_TRUE(r.ok());
}

TEST(BitReaderTest, SkipThenReadMatchesReadThenRead) {
    Bytes buf{0x13, 0x57, 0x9B, 0xDF};
    BitReader skipped(buf);
    BitReader read(buf);
    for (int width : {1, 7, 3, 5, 9}) {
        skipped.skip_bits(width);
        read.read_bits(width);
        EXPECT_EQ(skipped.bits_read(), read.bits_read());
        EXPECT_EQ(skipped.peek_bits(7), read.peek_bits(7));
    }
}

TEST(BitReaderTest, SkipZeroOrNegativeIsANoOp) {
    Bytes buf{0xFF};
    BitReader r(buf);
    r.skip_bits(0);
    r.skip_bits(-5);
    EXPECT_EQ(r.bits_read(), 0u);
    EXPECT_TRUE(r.ok());
}

TEST(BitReaderTest, SkipZeroOnEmptyStreamDoesNotOverrun) {
    BitReader r(nullptr, 0);
    r.skip_bits(0);
    EXPECT_TRUE(r.ok());
}

TEST(BitReaderTest, SkipCountAboveSixtyFourIsClamped) {
    Bytes buf(9, 0x00);
    buf[8] = 0x5A;
    BitReader r(buf);
    r.skip_bits(100);
    EXPECT_EQ(r.bits_read(), 64u);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.read_bits(8), 0x5Au);
}

TEST(BitReaderTest, SkipExactlyToTheEndIsOk) {
    Bytes buf{0xAB};
    BitReader r(buf);
    r.skip_bits(8);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(r.exhausted());
}

TEST(BitReaderTest, SkipPastTheEndSetsOverrun) {
    Bytes buf{0xAB};
    BitReader r(buf);
    r.skip_bits(9);
    EXPECT_TRUE(r.overrun());
    EXPECT_TRUE(r.exhausted());
}

// ---------------------------------------------------------------------------
// align_to_byte
// ---------------------------------------------------------------------------
TEST(BitReaderTest, AlignAtStartIsANoOp) {
    Bytes buf{0xAB};
    BitReader r(buf);
    r.align_to_byte();
    EXPECT_EQ(r.bits_read(), 0u);
}

TEST(BitReaderTest, AlignFromEveryOffsetInsideAByte) {
    Bytes buf{0x00, 0x00, 0xC3};
    for (int offset = 1; offset <= 8; ++offset) {
        BitReader r(buf);
        r.skip_bits(8 + offset);
        r.align_to_byte();
        EXPECT_EQ(r.bits_read(), 16u) << "offset " << offset;
        EXPECT_EQ(r.read_bits(8), 0xC3u) << "offset " << offset;
        EXPECT_TRUE(r.ok());
    }
}

TEST(BitReaderTest, AlignOnABoundaryIsANoOp) {
    Bytes buf{0xAB, 0xCD};
    BitReader r(buf);
    r.read_bits(8);
    r.align_to_byte();
    EXPECT_EQ(r.bits_read(), 8u);
    EXPECT_EQ(r.read_bits(8), 0xCDu);
}

TEST(BitReaderTest, AlignSpecificPositions) {
    Bytes buf{0x00, 0x00, 0x00};
    const std::pair<int, uint64_t> cases[] = {{1, 8}, {7, 8}, {8, 8}, {9, 16}, {15, 16}, {16, 16}};
    for (const auto& c : cases) {
        BitReader r(buf);
        r.skip_bits(c.first);
        r.align_to_byte();
        EXPECT_EQ(r.bits_read(), c.second) << "from " << c.first;
    }
}

TEST(BitReaderTest, AlignInTheLastByteLandsOnTheEndWithoutOverrun) {
    Bytes buf{0xFF};
    BitReader r(buf);
    r.read_bits(3);
    r.align_to_byte();
    EXPECT_EQ(r.bits_read(), 8u);
    EXPECT_TRUE(r.exhausted());
    EXPECT_TRUE(r.ok());
}

// ---------------------------------------------------------------------------
// Counter bookkeeping
// ---------------------------------------------------------------------------
TEST(BitReaderTest, BitsReadPlusRemainingIsConstantUntilTheEnd) {
    Bytes buf{0x01, 0x23, 0x45, 0x67, 0x89};
    BitReader r(buf);
    const int widths[] = {1, 2, 3, 4, 5, 6, 7, 8, 4};   // sums to 40
    for (int width : widths) {
        EXPECT_FALSE(r.exhausted());
        r.read_bits(width);
        EXPECT_EQ(r.bits_read() + r.bits_remaining(), 40u);
        EXPECT_EQ(r.exhausted(), r.bits_remaining() == 0);
    }
    EXPECT_TRUE(r.exhausted());
    EXPECT_TRUE(r.ok());
}
