// test_bitreader.cpp -- BitReader on its own, fed hand-built byte arrays.
//
// Covers MSB-first addressing, count clamping, peek (never flags overrun),
// skip, align, the sticky overrun flag and every counter. The model suite runs
// random operation sequences against a list-of-bits reference reader and
// compares every return value and counter after each step.
#include "BitReader.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <climits>
#include <cstdint>
#include <random>
#include <type_traits>
#include <vector>

using ts::Bytes;

namespace {

// The reference: the stream as a list of bits and a position that may run past it.
struct Model {
    std::vector<bool> bits;
    uint64_t pos = 0;
    bool overran = false;

    explicit Model(const Bytes& data) {
        for (uint8_t b : data)
            for (int k = 7; k >= 0; --k) bits.push_back(((b >> k) & 1) != 0);
    }

    uint64_t total() const { return bits.size(); }
    bool at(uint64_t p) const { return p < bits.size() && bits[p]; }

    uint64_t peek(int count) const {
        if (count <= 0) return 0;
        if (count > 64) count = 64;
        uint64_t r = 0;
        for (int i = 0; i < count; ++i) r = (r << 1) | (at(pos + i) ? 1 : 0);
        return r;
    }
    uint64_t read(int count) {
        if (count <= 0) return 0;
        const uint64_t r = peek(count);
        skip(count);
        return r;
    }
    void skip(int count) {
        if (count <= 0) return;
        if (count > 64) count = 64;
        if (pos + count > total()) overran = true;
        pos += count;
    }
    void align() { pos = (pos + 7) / 8 * 8; }
    uint64_t remaining() const { return pos < total() ? total() - pos : 0; }
    bool exhausted() const { return pos >= total(); }
};

void expectSameState(const BitReader& r, const Model& m, int step) {
    ASSERT_EQ(r.bits_read(), m.pos) << "step " << step;
    ASSERT_EQ(r.bits_remaining(), m.remaining()) << "step " << step;
    ASSERT_EQ(r.exhausted(), m.exhausted()) << "step " << step;
    ASSERT_EQ(r.overrun(), m.overran) << "step " << step;
    ASSERT_EQ(r.ok(), !m.overran) << "step " << step;
}

}  // namespace

// ===========================================================================
// Construction
// ===========================================================================
TEST(BitReaderTest, EmptyVectorIsExhaustedFromTheStart) {
    const Bytes empty;
    BitReader r(empty);
    EXPECT_EQ(r.bits_read(), 0u);
    EXPECT_EQ(r.bits_remaining(), 0u);
    EXPECT_TRUE(r.exhausted());
    EXPECT_FALSE(r.overrun());
    EXPECT_TRUE(r.ok());
}

TEST(BitReaderTest, NullPointerWithZeroSizeIsSafeForEveryOperation) {
    BitReader r(nullptr, 0);
    EXPECT_TRUE(r.exhausted());
    EXPECT_EQ(r.peek_bits(64), 0u);
    EXPECT_FALSE(r.overrun());
    r.align_to_byte();
    EXPECT_EQ(r.bits_read(), 0u);
    EXPECT_EQ(r.read_bits(9), 0u);
    EXPECT_TRUE(r.overrun());
    r.skip_bits(3);
    EXPECT_FALSE(r.read_bit());
    EXPECT_EQ(r.bits_remaining(), 0u);
}

TEST(BitReaderTest, FreshReaderOverNonEmptyBuffer) {
    const Bytes data = {0x12, 0x34, 0x56};
    BitReader r(data);
    EXPECT_EQ(r.bits_read(), 0u);
    EXPECT_EQ(r.bits_remaining(), 24u);
    EXPECT_FALSE(r.exhausted());
    EXPECT_TRUE(r.ok());
}

TEST(BitReaderTest, PointerAndVectorConstructorsAgree) {
    std::mt19937_64 rng(3);
    const Bytes data = ts::randomBytes(rng, 97);
    BitReader a(data);
    BitReader b(data.data(), data.size());
    while (!a.exhausted()) {
        const int n = 1 + static_cast<int>(rng() % 64);
        ASSERT_EQ(a.read_bits(n), b.read_bits(n));
        ASSERT_EQ(a.bits_read(), b.bits_read());
    }
    EXPECT_EQ(a.overrun(), b.overrun());
}

TEST(BitReaderTest, ReadsOnlyTheFirstSizeBytesOfALargerArray) {
    const uint8_t data[] = {0xAA, 0xBB, 0xFF, 0xFF};
    BitReader r(data, 2);
    EXPECT_EQ(r.bits_remaining(), 16u);
    EXPECT_EQ(r.read_bits(16), 0xAABBu);
    EXPECT_TRUE(r.exhausted());
    EXPECT_EQ(r.read_bits(8), 0u);   // the 0xFF after the view is never seen
    EXPECT_TRUE(r.overrun());
}

TEST(BitReaderTest, IsANonOwningView) {
    Bytes data = {0x00, 0x00};
    BitReader r(data);
    data[0] = 0xF0;   // same storage, so the reader sees the change
    EXPECT_EQ(r.read_bits(8), 0xF0u);
}

TEST(BitReaderTest, IsNotCopyable) {
    static_assert(!std::is_copy_constructible<BitReader>::value, "BitReader must not be copyable");
    static_assert(!std::is_copy_assignable<BitReader>::value, "BitReader must not be copy-assignable");
    SUCCEED();
}

TEST(BitReaderTest, PeekIsCallableOnAConstReader) {
    const Bytes data = {0xC0};
    const BitReader r(data);
    EXPECT_EQ(r.peek_bits(2), 3u);
    EXPECT_EQ(r.bits_read(), 0u);
    EXPECT_TRUE(r.ok());
}

// ===========================================================================
// read_bit / read_bits
// ===========================================================================
TEST(BitReaderTest, ReadBitIsMsbFirst) {
    const Bytes data = {0xB2};   // 1011 0010
    BitReader r(data);
    const bool want[] = {true, false, true, true, false, false, true, false};
    for (int i = 0; i < 8; ++i) EXPECT_EQ(r.read_bit(), want[i]) << "bit " << i;
    EXPECT_TRUE(r.exhausted());
    EXPECT_TRUE(r.ok());
}

TEST(BitReaderTest, EverySingleBitPositionIsAddressedCorrectly) {
    for (int pos = 0; pos < 64; ++pos) {
        Bytes data(8, 0);
        data[pos / 8] = static_cast<uint8_t>(0x80 >> (pos % 8));
        BitReader r(data);
        for (int i = 0; i < 64; ++i) ASSERT_EQ(r.read_bit(), i == pos) << "set " << pos << " read " << i;
    }
}

TEST(BitReaderTest, EveryByteValueReadsBackWholeAndBitByBit) {
    for (int v = 0; v < 256; ++v) {
        const Bytes data = {static_cast<uint8_t>(v)};
        BitReader whole(data);
        EXPECT_EQ(whole.read_bits(8), static_cast<uint64_t>(v));

        BitReader bits(data);
        uint64_t rebuilt = 0;
        for (int i = 0; i < 8; ++i) rebuilt = (rebuilt << 1) | (bits.read_bit() ? 1 : 0);
        EXPECT_EQ(rebuilt, static_cast<uint64_t>(v));
    }
}

TEST(BitReaderTest, ReadBitsAcrossByteBoundaries) {
    const Bytes data = {0xB8, 0x7E, 0xC0};   // 101 1100 00111111 0 11 000000
    BitReader r(data);
    EXPECT_EQ(r.read_bits(3), 0b101u);
    EXPECT_EQ(r.read_bits(4), 0b1100u);
    EXPECT_EQ(r.read_bits(8), 0b00111111u);
    EXPECT_EQ(r.read_bits(1), 0b0u);
    EXPECT_EQ(r.read_bits(2), 0b11u);
    EXPECT_EQ(r.bits_read(), 18u);
    EXPECT_EQ(r.bits_remaining(), 6u);
}

TEST(BitReaderTest, ReadSixtyFourBitsIsBigEndian) {
    const Bytes data = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    BitReader r(data);
    EXPECT_EQ(r.read_bits(64), 0x0123456789ABCDEFull);
    EXPECT_TRUE(r.exhausted());
    EXPECT_TRUE(r.ok());
}

TEST(BitReaderTest, ReadSixtyFourBitsFromEveryOffset) {
    const Bytes data = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x55};
    const unsigned __int128 all = (static_cast<unsigned __int128>(0x0123456789ABCDEFull) << 8) | 0x55;
    for (int off = 0; off <= 8; ++off) {
        BitReader r(data);
        r.skip_bits(off);
        const uint64_t want = static_cast<uint64_t>(all >> (8 - off));
        EXPECT_EQ(r.read_bits(64), want) << "offset " << off;
        EXPECT_TRUE(r.ok()) << "offset " << off;
    }
}

// ===========================================================================
// Count clamping, for read, peek and skip alike
// ===========================================================================
TEST(BitReaderTest, NonPositiveCountsReadNothing) {
    const Bytes data = {0xFF};
    BitReader r(data);
    for (int c : {0, -1, -8, -64, -65, INT_MIN}) {
        EXPECT_EQ(r.read_bits(c), 0u) << c;
        EXPECT_EQ(r.peek_bits(c), 0u) << c;
        r.skip_bits(c);
        EXPECT_EQ(r.bits_read(), 0u) << c;
    }
    EXPECT_TRUE(r.ok());
}

TEST(BitReaderTest, NonPositiveCountsOnAnEmptyStreamDoNotOverrun) {
    BitReader r(nullptr, 0);
    for (int c : {0, -1, INT_MIN}) {
        r.read_bits(c);
        r.skip_bits(c);
        r.peek_bits(c);
    }
    EXPECT_FALSE(r.overrun());
}

TEST(BitReaderTest, CountsAboveSixtyFourAreClamped) {
    Bytes data(16);
    for (int i = 0; i < 16; ++i) data[i] = static_cast<uint8_t>(0x10 * i + i);
    for (int c : {65, 66, 100, 1000, INT_MAX}) {
        BitReader a(data), b(data);
        EXPECT_EQ(a.peek_bits(c), b.peek_bits(64)) << c;
        EXPECT_EQ(a.read_bits(c), b.read_bits(64)) << c;
        EXPECT_EQ(a.bits_read(), 64u) << c;
        a.skip_bits(c);
        EXPECT_EQ(a.bits_read(), 128u) << c;
        EXPECT_TRUE(a.ok()) << c;
    }
}

// ===========================================================================
// peek_bits
// ===========================================================================
TEST(BitReaderTest, PeekMatchesTheFollowingRead) {
    std::mt19937_64 rng(4);
    const Bytes data = ts::randomBytes(rng, 256);
    BitReader r(data);
    while (r.bits_remaining() >= 64) {
        const int n = 1 + static_cast<int>(rng() % 64);
        const uint64_t peeked = r.peek_bits(n);
        ASSERT_EQ(r.read_bits(n), peeked);
    }
}

TEST(BitReaderTest, PeekDoesNotAdvanceAndIsRepeatable) {
    const Bytes data = {0xDE, 0xAD, 0xBE, 0xEF};
    BitReader r(data);
    r.skip_bits(5);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(r.peek_bits(13), r.peek_bits(13));
        EXPECT_EQ(r.bits_read(), 5u);
    }
}

TEST(BitReaderTest, ShorterPeekIsThePrefixOfALongerOne) {
    std::mt19937_64 rng(5);
    const Bytes data = ts::randomBytes(rng, 32);
    BitReader r(data);
    r.skip_bits(11);
    const uint64_t full = r.peek_bits(64);
    for (int n = 1; n <= 64; ++n) EXPECT_EQ(r.peek_bits(n), full >> (64 - n)) << n;
}

TEST(BitReaderTest, PeekPastTheEndZeroFillsWithoutFlaggingOverrun) {
    const Bytes data = {0xFF};
    BitReader r(data);
    EXPECT_EQ(r.peek_bits(16), 0xFF00u);
    EXPECT_EQ(r.peek_bits(64), 0xFF00000000000000ull);
    EXPECT_FALSE(r.overrun());
    EXPECT_EQ(r.bits_read(), 0u);
}

TEST(BitReaderTest, PeekWindowOverhangingTheEndHoldsTheTailLeftAligned) {
    const Bytes data = {0x00, 0x0B};   // last 4 bits: 1011
    BitReader r(data);
    r.skip_bits(12);
    EXPECT_EQ(r.peek_bits(15), 0b101100000000000u);
    EXPECT_EQ(r.peek_bits(9), 0b101100000u);
    EXPECT_TRUE(r.ok());
}

TEST(BitReaderTest, PeekOnAnExhaustedOrOverrunStreamReturnsZero) {
    const Bytes data = {0xFF};
    BitReader r(data);
    r.skip_bits(8);
    EXPECT_EQ(r.peek_bits(64), 0u);
    EXPECT_FALSE(r.overrun());
    r.skip_bits(20);
    EXPECT_EQ(r.peek_bits(64), 0u);
    EXPECT_TRUE(r.overrun());
}

// ===========================================================================
// Overrun
// ===========================================================================
TEST(BitReaderTest, ReadingExactlyToTheEndIsNotAnOverrun) {
    const Bytes data = {0xAB, 0xCD};
    BitReader r(data);
    r.read_bits(16);
    EXPECT_TRUE(r.exhausted());
    EXPECT_FALSE(r.overrun());
    EXPECT_TRUE(r.ok());
}

TEST(BitReaderTest, OneBitPastTheEndSetsOverrun) {
    const Bytes data = {0xAB};
    BitReader r(data);
    r.read_bits(8);
    EXPECT_FALSE(r.overrun());
    EXPECT_FALSE(r.read_bit());
    EXPECT_TRUE(r.overrun());
    EXPECT_FALSE(r.ok());
}

TEST(BitReaderTest, OverlongReadZeroFillsTheMissingBits) {
    const Bytes data = {0xFF};
    BitReader r(data);
    EXPECT_EQ(r.read_bits(12), 0xFF0u);
    EXPECT_TRUE(r.overrun());
}

TEST(BitReaderTest, OverlongReadStraddlingTheEnd) {
    const Bytes data = {0x00, 0x07};
    BitReader r(data);
    r.skip_bits(13);
    EXPECT_EQ(r.read_bits(5), 0b11100u);
    EXPECT_TRUE(r.overrun());
}

TEST(BitReaderTest, ReadingAnEmptyStreamSetsOverrun) {
    const Bytes data;
    BitReader r(data);
    EXPECT_FALSE(r.read_bit());
    EXPECT_TRUE(r.overrun());
}

TEST(BitReaderTest, AfterOverrunPositionKeepsCountingAndRemainingSaturates) {
    const Bytes data = {0x00};
    BitReader r(data);
    r.read_bits(10);
    EXPECT_EQ(r.bits_read(), 10u);
    EXPECT_EQ(r.bits_remaining(), 0u);
    EXPECT_TRUE(r.exhausted());
    r.read_bits(64);
    EXPECT_EQ(r.bits_read(), 74u);
    EXPECT_EQ(r.bits_remaining(), 0u);
}

TEST(BitReaderTest, OverrunIsStickyThroughEveryOperation) {
    const Bytes data = {0xFF, 0xFF};
    BitReader r(data);
    r.skip_bits(17);
    ASSERT_TRUE(r.overrun());
    r.peek_bits(8);
    r.read_bits(0);
    r.skip_bits(0);
    r.align_to_byte();
    r.read_bit();
    EXPECT_TRUE(r.overrun());
    EXPECT_FALSE(r.ok());
}

// ===========================================================================
// skip_bits
// ===========================================================================
TEST(BitReaderTest, SkipAdvancesWithoutDecoding) {
    const Bytes data = {0x0F, 0xF0};
    BitReader r(data);
    r.skip_bits(4);
    EXPECT_EQ(r.bits_read(), 4u);
    EXPECT_EQ(r.read_bits(8), 0xFFu);
}

TEST(BitReaderTest, SkipThenReadMatchesReadThenRead) {
    std::mt19937_64 rng(6);
    const Bytes data = ts::randomBytes(rng, 64);
    for (int trial = 0; trial < 1000; ++trial) {
        const int a = 1 + static_cast<int>(rng() % 64);
        const int b = 1 + static_cast<int>(rng() % 64);
        BitReader x(data), y(data);
        x.skip_bits(a);
        y.read_bits(a);
        ASSERT_EQ(x.read_bits(b), y.read_bits(b)) << a << "," << b;
        ASSERT_EQ(x.bits_read(), y.bits_read());
    }
}

TEST(BitReaderTest, SkipExactlyToTheEndIsOk) {
    const Bytes data = {0x00, 0x00, 0x00};
    BitReader r(data);
    r.skip_bits(24);
    EXPECT_TRUE(r.exhausted());
    EXPECT_TRUE(r.ok());
}

TEST(BitReaderTest, SkipPastTheEndSetsOverrun) {
    const Bytes data = {0x00, 0x00, 0x00};
    BitReader r(data);
    r.skip_bits(20);
    EXPECT_TRUE(r.ok());
    r.skip_bits(5);
    EXPECT_TRUE(r.overrun());
    EXPECT_EQ(r.bits_read(), 25u);
}

// ===========================================================================
// align_to_byte
// ===========================================================================
TEST(BitReaderTest, AlignAtStartIsANoOp) {
    const Bytes data = {0xAA};
    BitReader r(data);
    r.align_to_byte();
    EXPECT_EQ(r.bits_read(), 0u);
    EXPECT_EQ(r.read_bits(8), 0xAAu);
}

TEST(BitReaderTest, AlignFromEveryOffsetInsideEveryByte) {
    const Bytes data(8, 0x5A);
    for (int pos = 0; pos <= 64; ++pos) {
        BitReader r(data);
        r.skip_bits(pos);
        r.align_to_byte();
        EXPECT_EQ(r.bits_read(), static_cast<uint64_t>((pos + 7) / 8 * 8)) << "from " << pos;
        EXPECT_TRUE(r.ok()) << "from " << pos;
    }
}

TEST(BitReaderTest, AlignOnABoundaryIsANoOp) {
    const Bytes data = {0x11, 0x22, 0x33};
    BitReader r(data);
    r.read_bits(16);
    r.align_to_byte();
    r.align_to_byte();
    EXPECT_EQ(r.bits_read(), 16u);
    EXPECT_EQ(r.read_bits(8), 0x33u);
}

TEST(BitReaderTest, AlignInTheLastByteLandsOnTheEndWithoutOverrun) {
    const Bytes data = {0xFF, 0x80};
    BitReader r(data);
    r.read_bits(9);
    r.align_to_byte();
    EXPECT_TRUE(r.exhausted());
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.bits_read(), 16u);
}

TEST(BitReaderTest, AlignAfterOverrunRoundsUpAndStaysOverrun) {
    const Bytes data = {0xFF};
    BitReader r(data);
    r.read_bits(11);
    r.align_to_byte();
    EXPECT_EQ(r.bits_read(), 16u);
    EXPECT_TRUE(r.overrun());
}

// ===========================================================================
// Counters
// ===========================================================================
TEST(BitReaderTest, BitsReadPlusRemainingIsTheTotalUntilTheEnd) {
    std::mt19937_64 rng(7);
    const Bytes data = ts::randomBytes(rng, 50);
    BitReader r(data);
    while (!r.exhausted()) {
        EXPECT_EQ(r.bits_read() + r.bits_remaining(), 400u);
        r.read_bits(1 + static_cast<int>(rng() % 9));
    }
}

TEST(BitReaderTest, ExhaustedFlipsExactlyAtTheLastBit) {
    const Bytes data = {0x12, 0x34};
    BitReader r(data);
    for (int i = 0; i < 16; ++i) {
        EXPECT_FALSE(r.exhausted()) << i;
        EXPECT_EQ(r.bits_remaining(), static_cast<uint64_t>(16 - i));
        r.read_bit();
    }
    EXPECT_TRUE(r.exhausted());
    EXPECT_TRUE(r.ok());
}

TEST(BitReaderTest, LargeBufferReadAsWordsMatchesTheBytes) {
    std::mt19937_64 rng(8);
    const Bytes data = ts::randomBytes(rng, 1 << 18);
    BitReader r(data);
    for (std::size_t i = 0; i < data.size(); i += 8) {
        uint64_t want = 0;
        for (int k = 0; k < 8; ++k) want = (want << 8) | data[i + k];
        ASSERT_EQ(r.read_bits(64), want) << "word " << i / 8;
    }
    EXPECT_TRUE(r.exhausted());
    EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Every width at every bit offset: 8 x 64 cases, each against the model
// ===========================================================================
class BitReaderOffsetTest : public ::testing::TestWithParam<std::tuple<int, int>> {};

TEST_P(BitReaderOffsetTest, ReadPeekAndSkipMatchTheModel) {
    const int offset = std::get<0>(GetParam());
    const int width = std::get<1>(GetParam());
    std::mt19937_64 rng(offset * 97 + width);

    for (int trial = 0; trial < 8; ++trial) {
        const Bytes data = ts::randomBytes(rng, 1 + rng() % 12);   // often shorter than the read
        BitReader r(data);
        Model m(data);

        r.skip_bits(offset);
        m.skip(offset);
        ASSERT_EQ(r.peek_bits(width), m.peek(width));
        ASSERT_EQ(r.overrun(), m.overran) << "peek never overruns";
        ASSERT_EQ(r.read_bits(width), m.read(width));
        expectSameState(r, m, trial);
        r.skip_bits(width);
        m.skip(width);
        expectSameState(r, m, trial);
    }
}

INSTANTIATE_TEST_SUITE_P(EveryOffsetAndWidth, BitReaderOffsetTest,
                         ::testing::Combine(::testing::Range(0, 8), ::testing::Range(1, 65)));

// ===========================================================================
// Reads that run into the end, for every buffer size 0..8 and width 1..64
// ===========================================================================
class BitReaderEndTest : public ::testing::TestWithParam<std::tuple<int, int>> {};

TEST_P(BitReaderEndTest, ReadAtTheEndZeroFillsAndFlagsOnlyWhenPast) {
    const int size = std::get<0>(GetParam());
    const int width = std::get<1>(GetParam());
    const Bytes data(size, 0xFF);
    BitReader r(data);

    const uint64_t got = r.read_bits(width);
    const int real = std::min(width, size * 8);
    const uint64_t want = real == 0 ? 0 : ts::maskTo(~0ull, real) << (width - real);
    EXPECT_EQ(got, want);
    EXPECT_EQ(r.overrun(), width > size * 8);
    EXPECT_EQ(r.bits_read(), static_cast<uint64_t>(width));
}

INSTANTIATE_TEST_SUITE_P(SizesAndWidths, BitReaderEndTest,
                         ::testing::Combine(::testing::Range(0, 9), ::testing::Range(1, 65)));

// ===========================================================================
// Random operation sequences against the model: 200 seeds x 300 operations
// ===========================================================================
class BitReaderModelTest : public ::testing::TestWithParam<int> {};

TEST_P(BitReaderModelTest, RandomOperationsMatchTheModelAfterEveryStep) {
    std::mt19937_64 rng(static_cast<uint64_t>(GetParam()) * 104729 + 3);
    const Bytes data = ts::randomBytes(rng, rng() % 300);
    BitReader r(data);
    Model m(data);

    for (int step = 0; step < 300; ++step) {
        const int count = static_cast<int>(rng() % 75) - 5;   // -5..69: clamping included
        switch (rng() % 6) {
            case 0:
                ASSERT_EQ(r.read_bit(), m.read(1) != 0) << "step " << step;
                break;
            case 1:
            case 2:
                ASSERT_EQ(r.read_bits(count), m.read(count)) << "step " << step << " count " << count;
                break;
            case 3:
                ASSERT_EQ(r.peek_bits(count), m.peek(count)) << "step " << step << " count " << count;
                break;
            case 4:
                r.skip_bits(count);
                m.skip(count);
                break;
            case 5:
                r.align_to_byte();
                m.align();
                break;
        }
        expectSameState(r, m, step);
    }
}

INSTANTIATE_TEST_SUITE_P(Seeds, BitReaderModelTest, ::testing::Range(0, 200));
