// test_bitwriter.cpp -- BitWriter on its own: packing order, clamping, flush.
// Expected bytes are written out by hand; BitReader is not involved.
#include "BitWriter.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>
#include <vector>

using Bytes = std::vector<uint8_t>;

TEST(BitWriterTest, FreshWriterHasWrittenNothing) {
    Bytes buf;
    BitWriter w(buf);
    EXPECT_EQ(w.bits_written(), 0u);
    EXPECT_EQ(w.bits_pending(), 0);
    EXPECT_TRUE(buf.empty());
}

TEST(BitWriterTest, DestroyingAnUnusedWriterAddsNoBytes) {
    Bytes buf;
    { BitWriter w(buf); }
    EXPECT_TRUE(buf.empty());
}

TEST(BitWriterTest, SingleSetBitLandsInBit7) {
    Bytes buf;
    { BitWriter w(buf); w.write_bit(true); }
    EXPECT_EQ(buf, Bytes{0x80});
}

TEST(BitWriterTest, SingleClearBitStillProducesAByte) {
    Bytes buf;
    { BitWriter w(buf); w.write_bit(false); }
    EXPECT_EQ(buf, Bytes{0x00});
}

TEST(BitWriterTest, BitsArePackedMsbFirst) {
    Bytes buf;
    {
        BitWriter w(buf);
        for (bool b : {true, false, true, true, false, false, true, false}) w.write_bit(b);
    }
    EXPECT_EQ(buf, Bytes{0xB2});
}

TEST(BitWriterTest, PartialByteIsHeldBackUntilItFills) {
    Bytes buf;
    BitWriter w(buf);
    for (int i = 0; i < 7; ++i) w.write_bit(true);
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(w.bits_pending(), 7);
    EXPECT_EQ(w.bits_written(), 7u);

    w.write_bit(true);
    EXPECT_EQ(buf, Bytes{0xFF});
    EXPECT_EQ(w.bits_pending(), 0);
    EXPECT_EQ(w.bits_written(), 8u);
}

TEST(BitWriterTest, BitsPendingCyclesThroughZeroToSeven) {
    Bytes buf;
    BitWriter w(buf);
    for (int i = 0; i < 24; ++i) {
        EXPECT_EQ(w.bits_pending(), i % 8) << "before bit " << i;
        w.write_bit(i % 3 == 0);
        EXPECT_EQ(w.bits_pending(), (i + 1) % 8) << "after bit " << i;
        EXPECT_EQ(buf.size(), static_cast<std::size_t>((i + 1) / 8)) << "after bit " << i;
        EXPECT_EQ(w.bits_written(), static_cast<uint64_t>(i + 1));
    }
}

TEST(BitWriterTest, WriteBitsIgnoresBitsAboveCount) {
    Bytes a, b;
    { BitWriter w(a); w.write_bits(0xFF, 4); }
    { BitWriter w(b); w.write_bits(0xF0, 4); }
    EXPECT_EQ(a, Bytes{0xF0});
    EXPECT_EQ(b, Bytes{0x00});
}

TEST(BitWriterTest, WriteBitsWithHighJunkMatchesMaskedValue) {
    Bytes junk, clean;
    { BitWriter w(junk);  w.write_bits(0xFFFFFFFFFFFFFF05ull, 5); }
    { BitWriter w(clean); w.write_bits(0x05, 5); }
    EXPECT_EQ(junk, clean);
    EXPECT_EQ(junk, Bytes{0x28});   // 00101 000
}

TEST(BitWriterTest, SixtyFourBitValueIsWrittenBigEndian) {
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(0xDEADBEEFCAFEBABEull, 64);
        EXPECT_EQ(w.bits_written(), 64u);
        EXPECT_EQ(w.bits_pending(), 0);
    }
    EXPECT_EQ(buf, (Bytes{0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE}));
}

TEST(BitWriterTest, CountZeroWritesNothing) {
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(0xFFFF, 0);
        EXPECT_EQ(w.bits_written(), 0u);
        EXPECT_EQ(w.bits_pending(), 0);
    }
    EXPECT_TRUE(buf.empty());
}

TEST(BitWriterTest, NegativeCountWritesNothing) {
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(0xFFFF, -5);
        w.write_bits(0xFFFF, -1000);
        EXPECT_EQ(w.bits_written(), 0u);
    }
    EXPECT_TRUE(buf.empty());
}

TEST(BitWriterTest, CountAboveSixtyFourIsClampedToSixtyFour) {
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(0x0123456789ABCDEFull, 100);
        EXPECT_EQ(w.bits_written(), 64u);
        EXPECT_EQ(w.bits_pending(), 0);
    }
    EXPECT_EQ(buf, (Bytes{0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}));
}

TEST(BitWriterTest, CountOfSixtyFiveBehavesLikeSixtyFour) {
    Bytes a, b;
    { BitWriter w(a); w.write_bits(0xA5A5A5A5A5A5A5A5ull, 65); }
    { BitWriter w(b); w.write_bits(0xA5A5A5A5A5A5A5A5ull, 64); }
    EXPECT_EQ(a, b);
}

TEST(BitWriterTest, AppendsToExistingBufferContents) {
    Bytes buf{0x11, 0x22};
    { BitWriter w(buf); w.write_bits(0xAB, 8); w.write_bit(true); }
    EXPECT_EQ(buf, (Bytes{0x11, 0x22, 0xAB, 0x80}));
}

TEST(BitWriterTest, BitsWrittenIgnoresPreexistingBufferContents) {
    Bytes buf{0x11, 0x22, 0x33};
    BitWriter w(buf);
    EXPECT_EQ(w.bits_written(), 0u);
    w.write_bits(0x3, 2);
    EXPECT_EQ(w.bits_written(), 2u);
}

TEST(BitWriterTest, MixedWidthsCrossByteBoundaries) {
    // 101 | 1100110 | 011011  ->  10111001 10011011
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(0b101, 3);
        w.write_bits(0b1100110, 7);
        w.write_bits(0b011011, 6);
        EXPECT_EQ(w.bits_written(), 16u);
    }
    EXPECT_EQ(buf, (Bytes{0xB9, 0x9B}));
}

TEST(BitWriterTest, LeadingZerosOfAValueAreWritten) {
    Bytes buf;
    { BitWriter w(buf); w.write_bits(0b0001, 4); w.write_bits(0b0001, 4); }
    EXPECT_EQ(buf, Bytes{0x11});
}

TEST(BitWriterTest, FlushPadsPartialByteWithZeros) {
    Bytes buf;
    BitWriter w(buf);
    w.write_bits(0b111, 3);
    w.flush();
    EXPECT_EQ(buf, Bytes{0xE0});
    EXPECT_EQ(w.bits_pending(), 0);
}

TEST(BitWriterTest, FlushDoesNotCountPaddingInBitsWritten) {
    Bytes buf;
    BitWriter w(buf);
    w.write_bits(0b1, 1);
    w.flush();
    EXPECT_EQ(w.bits_written(), 1u);
}

TEST(BitWriterTest, FlushWhenAlignedIsANoOp) {
    Bytes buf;
    BitWriter w(buf);
    w.flush();
    EXPECT_TRUE(buf.empty());

    w.write_bits(0xAB, 8);
    w.flush();
    EXPECT_EQ(buf, Bytes{0xAB});
    EXPECT_EQ(w.bits_written(), 8u);
}

TEST(BitWriterTest, DoubleFlushAddsOnlyOneByte) {
    Bytes buf;
    BitWriter w(buf);
    w.write_bit(true);
    w.flush();
    w.flush();
    EXPECT_EQ(buf, Bytes{0x80});
}

TEST(BitWriterTest, WritingAfterFlushStartsAFreshByte) {
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(0b11, 2);
        w.flush();
        w.write_bits(0b01, 2);
        EXPECT_EQ(w.bits_written(), 4u);
        EXPECT_EQ(w.bits_pending(), 2);
    }
    EXPECT_EQ(buf, (Bytes{0xC0, 0x40}));
}

TEST(BitWriterTest, DestructorFlushesPendingBits) {
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(0xAB, 8);
        w.write_bits(0b1011, 4);
        EXPECT_EQ(buf, Bytes{0xAB});
    }
    EXPECT_EQ(buf, (Bytes{0xAB, 0xB0}));
}

TEST(BitWriterTest, BufferSizeIsCeilOfBitsOverEight) {
    for (int bits = 0; bits <= 40; ++bits) {
        Bytes buf;
        { BitWriter w(buf); for (int i = 0; i < bits; ++i) w.write_bit(true); }
        EXPECT_EQ(buf.size(), static_cast<std::size_t>((bits + 7) / 8)) << bits << " bits";
    }
}

TEST(BitWriterTest, IsNotCopyable) {
    EXPECT_FALSE(std::is_copy_constructible<BitWriter>::value);
    EXPECT_FALSE(std::is_copy_assignable<BitWriter>::value);
}

// ---------------------------------------------------------------------------
// Every width 1..64: all-ones value -> top `w` bits set, the rest zero.
// ---------------------------------------------------------------------------
class BitWriterWidthTest : public ::testing::TestWithParam<int> {};

TEST_P(BitWriterWidthTest, AllOnesFillsExactlyTheTopWidthBits) {
    const int width = GetParam();
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(~0ull, width);
        EXPECT_EQ(w.bits_written(), static_cast<uint64_t>(width));
        EXPECT_EQ(w.bits_pending(), width % 8);
    }

    ASSERT_EQ(buf.size(), static_cast<std::size_t>((width + 7) / 8));
    for (int i = 0; i < width / 8; ++i) {
        EXPECT_EQ(buf[i], 0xFF) << "byte " << i;
    }
    if (width % 8 != 0) {
        const uint8_t tail = static_cast<uint8_t>(0xFF << (8 - width % 8));
        EXPECT_EQ(buf.back(), tail);
    }
}

TEST_P(BitWriterWidthTest, SingleLowBitLandsAtTheLastWrittenPosition) {
    const int width = GetParam();
    Bytes buf;
    { BitWriter w(buf); w.write_bits(1, width); }

    ASSERT_EQ(buf.size(), static_cast<std::size_t>((width + 7) / 8));
    const int last_bit = width - 1;   // stream position of the set bit
    for (std::size_t i = 0; i < buf.size(); ++i) {
        const uint8_t want = (static_cast<int>(i) == last_bit / 8)
                                 ? static_cast<uint8_t>(0x80 >> (last_bit % 8))
                                 : 0;
        EXPECT_EQ(buf[i], want) << "byte " << i;
    }
}

INSTANTIATE_TEST_SUITE_P(AllWidths, BitWriterWidthTest, ::testing::Range(1, 65));
