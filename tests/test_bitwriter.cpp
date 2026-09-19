// test_bitwriter.cpp -- BitWriter on its own: MSB-first packing, clamping of the
// count, flush and destructor padding, and the two counters.
//
// Hand-written cases pin exact bytes; the parameterized suites sweep every
// width at every bit offset; the model suite replays thousands of random
// operations against a plain list-of-bits model and compares after each one.
#include "BitWriter.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <climits>
#include <cstdint>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

using ts::Bytes;
using ts::maskTo;

namespace {

// The reference: the partial byte kept as a list of bits, packed into a whole
// byte only once there are eight. Knows nothing about shifting into a byte.
struct Model {
    Bytes bytes;               // what the buffer must hold: whole bytes only
    std::vector<bool> partial; // bits of the byte not yet complete, first one first
    uint64_t written = 0;      // bits handed over, padding excluded

    void bit(bool b) {
        partial.push_back(b);
        ++written;
        if (partial.size() == 8) pack();
    }
    void value(uint64_t v, int count) {
        if (count <= 0) return;
        if (count > 64) count = 64;
        for (int i = count - 1; i >= 0; --i) bit(((v >> i) & 1) != 0);
    }
    void flush() {
        if (partial.empty()) return;
        while (partial.size() < 8) partial.push_back(false);
        pack();
    }
    int pending() const { return static_cast<int>(partial.size()); }
    const Bytes& buffer() const { return bytes; }

private:
    void pack() {
        int byte = 0;
        for (bool b : partial) byte = byte * 2 + (b ? 1 : 0);
        bytes.push_back(static_cast<uint8_t>(byte));
        partial.clear();
    }
};

std::string hex(const Bytes& b) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (uint8_t x : b) {
        s += digits[x >> 4];
        s += digits[x & 15];
        s += ' ';
    }
    return s;
}

}  // namespace

// ===========================================================================
// Construction and lifetime
// ===========================================================================
TEST(BitWriterTest, FreshWriterHasWrittenNothing) {
    Bytes buf;
    BitWriter w(buf);
    EXPECT_EQ(w.bits_written(), 0u);
    EXPECT_EQ(w.bits_pending(), 0);
    EXPECT_TRUE(buf.empty());
}

TEST(BitWriterTest, ConstructingDoesNotTouchExistingContents) {
    Bytes buf = {0xDE, 0xAD};
    {
        BitWriter w(buf);
        EXPECT_EQ(buf, (Bytes{0xDE, 0xAD}));
    }
    EXPECT_EQ(buf, (Bytes{0xDE, 0xAD}));
}

TEST(BitWriterTest, DestroyingAnUnusedWriterAddsNoBytes) {
    Bytes buf;
    { BitWriter w(buf); }
    EXPECT_TRUE(buf.empty());
}

TEST(BitWriterTest, IsNotCopyable) {
    static_assert(!std::is_copy_constructible<BitWriter>::value, "BitWriter must not be copyable");
    static_assert(!std::is_copy_assignable<BitWriter>::value, "BitWriter must not be copy-assignable");
    SUCCEED();
}

TEST(BitWriterTest, IsNotDefaultConstructible) {
    static_assert(!std::is_default_constructible<BitWriter>::value, "BitWriter needs a buffer");
    SUCCEED();
}

// ===========================================================================
// write_bit
// ===========================================================================
TEST(BitWriterTest, SingleSetBitLandsInBit7) {
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bit(true);
    }
    EXPECT_EQ(buf, (Bytes{0x80}));
}

TEST(BitWriterTest, SingleClearBitStillProducesAByteOnFlush) {
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bit(false);
    }
    EXPECT_EQ(buf, (Bytes{0x00}));
}

TEST(BitWriterTest, BitsArePackedMsbFirst) {
    Bytes buf;
    BitWriter w(buf);
    for (bool b : {true, false, true, true, false, false, true, false}) w.write_bit(b);
    EXPECT_EQ(buf, (Bytes{0xB2}));
}

TEST(BitWriterTest, EighthBitCompletesTheByteWithoutAFlush) {
    Bytes buf;
    BitWriter w(buf);
    for (int i = 0; i < 7; ++i) w.write_bit(true);
    EXPECT_TRUE(buf.empty());
    w.write_bit(true);
    EXPECT_EQ(buf, (Bytes{0xFF}));
    EXPECT_EQ(w.bits_pending(), 0);
}

TEST(BitWriterTest, PartialByteIsHeldBackUntilItFills) {
    Bytes buf;
    BitWriter w(buf);
    for (int i = 0; i < 12; ++i) w.write_bit(i % 3 == 0);
    ASSERT_EQ(buf.size(), 1u);
    EXPECT_EQ(buf[0], 0x92);   // 1001 0010
    EXPECT_EQ(w.bits_pending(), 4);
    w.flush();
    EXPECT_EQ(buf, (Bytes{0x92, 0x40}));   // 0100 + pad
}

TEST(BitWriterTest, BitsPendingCyclesThroughZeroToSeven) {
    Bytes buf;
    BitWriter w(buf);
    for (int i = 0; i < 200; ++i) {
        EXPECT_EQ(w.bits_pending(), i % 8) << "after " << i << " bits";
        EXPECT_EQ(buf.size(), static_cast<std::size_t>(i / 8));
        w.write_bit((i & 1) != 0);
    }
}

TEST(BitWriterTest, BitsWrittenCountsEveryBit) {
    Bytes buf;
    BitWriter w(buf);
    for (uint64_t i = 1; i <= 1000; ++i) {
        w.write_bit(i % 7 == 0);
        EXPECT_EQ(w.bits_written(), i);
    }
}

TEST(BitWriterTest, AlternatingBitsGiveFiftyFiveAndAa) {
    Bytes a, b;
    {
        BitWriter wa(a), wb(b);
        for (int i = 0; i < 64; ++i) {
            wa.write_bit(i % 2 == 1);
            wb.write_bit(i % 2 == 0);
        }
    }
    EXPECT_EQ(a, Bytes(8, 0x55));
    EXPECT_EQ(b, Bytes(8, 0xAA));
}

// ===========================================================================
// write_bits
// ===========================================================================
TEST(BitWriterTest, WriteBitsIgnoresBitsAboveCount) {
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(0xFFFFFFFFFFFFFF05ull, 4);   // only 0101 counts
    }
    EXPECT_EQ(buf, (Bytes{0x50}));
}

TEST(BitWriterTest, SixtyFourBitValueIsWrittenBigEndian) {
    Bytes buf;
    BitWriter w(buf);
    w.write_bits(0x0123456789ABCDEFull, 64);
    EXPECT_EQ(buf, (Bytes{0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}));
    EXPECT_EQ(w.bits_pending(), 0);
    EXPECT_EQ(w.bits_written(), 64u);
}

TEST(BitWriterTest, LeadingZerosOfAValueAreWritten) {
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(1, 8);
        w.write_bits(0, 5);
        w.write_bits(1, 3);
    }
    EXPECT_EQ(buf, (Bytes{0x01, 0x01}));
}

TEST(BitWriterTest, MixedWidthsCrossByteBoundaries) {
    // 101 | 1100 | 0011 1111 | 0 | 11 -> 10111000 01111110 11(000000)
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(0b101, 3);
        w.write_bits(0b1100, 4);
        w.write_bits(0b00111111, 8);
        w.write_bits(0b0, 1);
        w.write_bits(0b11, 2);
        EXPECT_EQ(w.bits_written(), 18u);
    }
    EXPECT_EQ(buf, (Bytes{0xB8, 0x7E, 0xC0}));
}

TEST(BitWriterTest, WriteBitsOfOneEqualsWriteBit) {
    Bytes a, b;
    std::mt19937_64 rng(1);
    {
        BitWriter wa(a), wb(b);
        for (int i = 0; i < 5000; ++i) {
            const uint64_t v = rng();
            wa.write_bits(v, 1);
            wb.write_bit((v & 1) != 0);
        }
    }
    EXPECT_EQ(a, b);
}

TEST(BitWriterTest, SplittingAValueAnywhereGivesTheSameBytes) {
    std::mt19937_64 rng(2);
    for (int trial = 0; trial < 2000; ++trial) {
        const uint64_t v = rng();
        const int split = static_cast<int>(rng() % 65);   // 0..64
        Bytes whole, parts;
        {
            BitWriter a(whole), b(parts);
            a.write_bits(v, 64);
            b.write_bits(split == 64 ? 0 : v >> split, 64 - split);   // v >> 64 is undefined
            b.write_bits(maskTo(v, split), split);
        }
        ASSERT_EQ(whole, parts) << "split at " << split;
    }
}

// ===========================================================================
// Count clamping
// ===========================================================================
TEST(BitWriterTest, CountZeroWritesNothing) {
    Bytes buf;
    BitWriter w(buf);
    w.write_bits(~0ull, 0);
    EXPECT_EQ(w.bits_written(), 0u);
    EXPECT_EQ(w.bits_pending(), 0);
    w.flush();
    EXPECT_TRUE(buf.empty());
}

TEST(BitWriterTest, NegativeCountsWriteNothing) {
    Bytes buf;
    BitWriter w(buf);
    for (int c : {-1, -2, -7, -8, -63, -64, -65, -1000, INT_MIN}) w.write_bits(~0ull, c);
    EXPECT_EQ(w.bits_written(), 0u);
    w.flush();
    EXPECT_TRUE(buf.empty());
}

TEST(BitWriterTest, CountsAboveSixtyFourAreClampedToSixtyFour) {
    for (int c : {65, 66, 100, 128, 1000, INT_MAX}) {
        Bytes clamped, exact;
        {
            BitWriter a(clamped), b(exact);
            a.write_bits(0x8000000000000001ull, c);
            b.write_bits(0x8000000000000001ull, 64);
            EXPECT_EQ(a.bits_written(), 64u) << "count " << c;
        }
        EXPECT_EQ(clamped, exact) << "count " << c;
    }
}

TEST(BitWriterTest, NoOpWritesDoNotDisturbAPartialByte) {
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(0b101, 3);
        w.write_bits(0xFF, 0);
        w.write_bits(0xFF, -5);
        EXPECT_EQ(w.bits_pending(), 3);
        w.write_bits(0b11111, 5);
    }
    EXPECT_EQ(buf, (Bytes{0xBF}));
}

// ===========================================================================
// Appending to an existing buffer
// ===========================================================================
TEST(BitWriterTest, AppendsToExistingBufferContents) {
    Bytes buf = {0x11, 0x22};
    {
        BitWriter w(buf);
        w.write_bits(0xAB, 8);
        w.write_bit(true);
    }
    EXPECT_EQ(buf, (Bytes{0x11, 0x22, 0xAB, 0x80}));
}

TEST(BitWriterTest, BitsWrittenIgnoresPreexistingBufferContents) {
    Bytes buf(100, 0x77);
    BitWriter w(buf);
    w.write_bits(5, 3);
    EXPECT_EQ(w.bits_written(), 3u);
    EXPECT_EQ(w.bits_pending(), 3);
}

TEST(BitWriterTest, WritersUsedOneAfterAnotherConcatenateByteAligned) {
    Bytes buf;
    { BitWriter w(buf); w.write_bits(0b1, 1); }
    { BitWriter w(buf); w.write_bits(0b11, 2); }
    { BitWriter w(buf); w.write_bits(0xAB, 8); }
    EXPECT_EQ(buf, (Bytes{0x80, 0xC0, 0xAB}));
}

TEST(BitWriterTest, TwoWritersOnTwoBuffersAreIndependent) {
    Bytes a, b;
    {
        BitWriter wa(a), wb(b);
        for (int i = 0; i < 100; ++i) {
            wa.write_bit(true);
            if (i % 2 == 0) wb.write_bit(false);
        }
        EXPECT_EQ(wa.bits_written(), 100u);
        EXPECT_EQ(wb.bits_written(), 50u);
    }
    EXPECT_EQ(a.size(), 13u);
    EXPECT_EQ(b.size(), 7u);
    EXPECT_EQ(b, Bytes(7, 0x00));
}

// ===========================================================================
// flush and the destructor
// ===========================================================================
TEST(BitWriterTest, FlushPadsPartialByteWithZeros) {
    Bytes buf;
    BitWriter w(buf);
    w.write_bits(0b111, 3);
    w.flush();
    EXPECT_EQ(buf, (Bytes{0xE0}));
    EXPECT_EQ(w.bits_pending(), 0);
}

TEST(BitWriterTest, FlushDoesNotCountPaddingInBitsWritten) {
    Bytes buf;
    BitWriter w(buf);
    w.write_bits(0b10101, 5);
    w.flush();
    EXPECT_EQ(w.bits_written(), 5u);
}

TEST(BitWriterTest, FlushWhenAlignedIsANoOp) {
    Bytes buf;
    BitWriter w(buf);
    w.flush();
    EXPECT_TRUE(buf.empty());
    w.write_bits(0xAB, 8);
    w.flush();
    EXPECT_EQ(buf, (Bytes{0xAB}));
    EXPECT_EQ(w.bits_written(), 8u);
}

TEST(BitWriterTest, RepeatedFlushesAddOnlyOneByte) {
    Bytes buf;
    BitWriter w(buf);
    w.write_bit(true);
    for (int i = 0; i < 10; ++i) w.flush();
    EXPECT_EQ(buf, (Bytes{0x80}));
}

TEST(BitWriterTest, WritingAfterFlushStartsAFreshByte) {
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(0b1, 1);
        w.flush();
        w.write_bits(0b1, 1);
        EXPECT_EQ(w.bits_written(), 2u);
    }
    EXPECT_EQ(buf, (Bytes{0x80, 0x80}));
}

TEST(BitWriterTest, DestructorFlushesPendingBits) {
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(0b1011, 4);
        EXPECT_TRUE(buf.empty());
    }
    EXPECT_EQ(buf, (Bytes{0xB0}));
}

TEST(BitWriterTest, DestructorAfterFlushAddsNothing) {
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(0b1011, 4);
        w.flush();
    }
    EXPECT_EQ(buf, (Bytes{0xB0}));
}

TEST(BitWriterTest, BufferSizeIsCeilOfBitsOverEight) {
    for (int n = 0; n <= 256; ++n) {
        Bytes buf;
        {
            BitWriter w(buf);
            for (int i = 0; i < n; ++i) w.write_bit(true);
        }
        EXPECT_EQ(buf.size(), static_cast<std::size_t>((n + 7) / 8)) << n << " bits";
    }
}

TEST(BitWriterTest, MillionBitsOfOnes) {
    const int n = 1000003;
    Bytes buf;
    {
        BitWriter w(buf);
        for (int i = 0; i < n; ++i) w.write_bit(true);
        EXPECT_EQ(w.bits_written(), static_cast<uint64_t>(n));
    }
    ASSERT_EQ(buf.size(), static_cast<std::size_t>((n + 7) / 8));
    for (std::size_t i = 0; i + 1 < buf.size(); ++i) ASSERT_EQ(buf[i], 0xFF) << "byte " << i;
    EXPECT_EQ(buf.back(), 0xE0);   // 3 ones + 5 pad
}

// ===========================================================================
// One write of every width 1..64
// ===========================================================================
class BitWriterWidthTest : public ::testing::TestWithParam<int> {};

TEST_P(BitWriterWidthTest, AllOnesFillsExactlyTheTopWidthBits) {
    const int width = GetParam();
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(~0ull, width);
    }
    ASSERT_EQ(buf.size(), static_cast<std::size_t>((width + 7) / 8));
    for (int i = 0; i < static_cast<int>(buf.size()) * 8; ++i) {
        const bool set = (buf[i / 8] >> (7 - i % 8)) & 1;
        EXPECT_EQ(set, i < width) << "bit " << i;
    }
}

TEST_P(BitWriterWidthTest, SingleLowBitLandsAtTheLastWrittenPosition) {
    const int width = GetParam();
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(1, width);
    }
    const int last = width - 1;
    for (int i = 0; i < static_cast<int>(buf.size()) * 8; ++i) {
        EXPECT_EQ(((buf[i / 8] >> (7 - i % 8)) & 1) != 0, i == last) << "bit " << i;
    }
}

TEST_P(BitWriterWidthTest, TopBitOfTheWidthLandsFirst) {
    const int width = GetParam();
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(1ull << (width - 1), width);
    }
    ASSERT_FALSE(buf.empty());
    EXPECT_EQ(buf[0] & 0x80, 0x80);
    for (std::size_t i = 1; i < buf.size(); ++i) EXPECT_EQ(buf[i], 0) << "byte " << i;
    EXPECT_EQ(buf[0] & 0x7F, 0);
}

TEST_P(BitWriterWidthTest, JunkAboveTheWidthNeverLeaks) {
    const int width = GetParam();
    std::mt19937_64 rng(1000 + width);
    for (int trial = 0; trial < 200; ++trial) {
        const uint64_t v = rng();
        Bytes raw, masked;
        {
            BitWriter a(raw), b(masked);
            a.write_bits(v, width);
            b.write_bits(maskTo(v, width), width);
        }
        ASSERT_EQ(raw, masked) << "value " << v;
    }
}

TEST_P(BitWriterWidthTest, CountersAfterOneWrite) {
    const int width = GetParam();
    Bytes buf;
    BitWriter w(buf);
    w.write_bits(0, width);
    EXPECT_EQ(w.bits_written(), static_cast<uint64_t>(width));
    EXPECT_EQ(w.bits_pending(), width % 8);
    EXPECT_EQ(buf.size(), static_cast<std::size_t>(width / 8));
}

TEST_P(BitWriterWidthTest, RepeatedWritesMatchTheModel) {
    const int width = GetParam();
    std::mt19937_64 rng(2000 + width);
    Model m;
    Bytes buf;
    {
        BitWriter w(buf);
        for (int i = 0; i < 300; ++i) {
            const uint64_t v = rng();
            w.write_bits(v, width);
            m.value(v, width);
        }
        EXPECT_EQ(w.bits_written(), m.written);
        w.flush();
        m.flush();
    }
    EXPECT_EQ(buf, m.buffer());
}

INSTANTIATE_TEST_SUITE_P(AllWidths, BitWriterWidthTest, ::testing::Range(1, 65));

// ===========================================================================
// Every width at every bit offset inside a byte: 8 x 64 cases
// ===========================================================================
class BitWriterOffsetTest : public ::testing::TestWithParam<std::tuple<int, int>> {};

TEST_P(BitWriterOffsetTest, ValueAfterOffsetBitsMatchesTheModel) {
    const int offset = std::get<0>(GetParam());
    const int width = std::get<1>(GetParam());
    std::mt19937_64 rng(offset * 131 + width);

    for (int trial = 0; trial < 8; ++trial) {
        const uint64_t lead = rng();
        const uint64_t v = rng();
        Model m;
        Bytes buf;
        {
            BitWriter w(buf);
            w.write_bits(lead, offset);
            m.value(lead, offset);
            w.write_bits(v, width);
            m.value(v, width);
            EXPECT_EQ(w.bits_written(), static_cast<uint64_t>(offset + width));
            EXPECT_EQ(w.bits_pending(), (offset + width) % 8);
            EXPECT_EQ(buf, m.buffer()) << "before flush";
            w.flush();
            m.flush();
        }
        ASSERT_EQ(buf, m.buffer()) << "got " << hex(buf) << " want " << hex(m.buffer());
    }
}

INSTANTIATE_TEST_SUITE_P(EveryOffsetAndWidth, BitWriterOffsetTest,
                         ::testing::Combine(::testing::Range(0, 8), ::testing::Range(1, 65)));

// ===========================================================================
// Random operation sequences against the model: 200 seeds x 400 operations
// ===========================================================================
class BitWriterModelTest : public ::testing::TestWithParam<int> {};

TEST_P(BitWriterModelTest, RandomOperationsMatchTheModelAfterEveryStep) {
    std::mt19937_64 rng(static_cast<uint64_t>(GetParam()) * 7919 + 17);

    Model m;
    const std::size_t prefix = rng() % 4;   // the writer appends after whatever is there
    for (std::size_t i = 0; i < prefix; ++i) m.bytes.push_back(static_cast<uint8_t>(rng()));

    Bytes buf = m.bytes;
    {
        BitWriter w(buf);
        for (int step = 0; step < 400; ++step) {
            const int op = static_cast<int>(rng() % 10);
            if (op < 3) {
                const bool b = (rng() & 1) != 0;
                w.write_bit(b);
                m.bit(b);
            } else if (op < 9) {
                const uint64_t v = rng();
                const int count = static_cast<int>(rng() % 75) - 5;   // -5..69: clamping included
                w.write_bits(v, count);
                m.value(v, count);
            } else {
                w.flush();
                m.flush();
            }
            ASSERT_EQ(w.bits_written(), m.written) << "step " << step;
            ASSERT_EQ(w.bits_pending(), m.pending()) << "step " << step;
            ASSERT_EQ(buf, m.buffer()) << "step " << step;
        }
    }
    m.flush();
    EXPECT_EQ(buf, m.buffer()) << "after destruction";
}

INSTANTIATE_TEST_SUITE_P(Seeds, BitWriterModelTest, ::testing::Range(0, 200));
