// test_bitio.cpp -- BitWriter -> BitReader round trips.
// No Huffman code is involved: if this passes, bit I/O is off the suspect list.
//
// The fixture writes 50,000 random (value, width) pairs once and checks one
// property per test. The parameterized suites repeat the round trip for every
// width, for random flush/align segmentations and for mixed bit/word writes.
#include "core/BitReader.hpp"
#include "core/BitWriter.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <vector>

using ts::Bytes;
using ts::maskTo;

namespace {

struct Pair {
    uint64_t raw;     // what gets handed to write_bits, junk above the width included
    uint64_t value;   // what must come back out
    int width;
};

std::vector<Pair> randomPairs(std::mt19937_64& rng, int n, int minWidth = 1, int maxWidth = 64) {
    std::vector<Pair> pairs;
    pairs.reserve(n);
    for (int i = 0; i < n; ++i) {
        const int width = minWidth + static_cast<int>(rng() % (maxWidth - minWidth + 1));
        const uint64_t raw = rng();
        pairs.push_back({raw, maskTo(raw, width), width});
    }
    return pairs;
}

}  // namespace

// ===========================================================================
// 50,000 random pairs, fixed seed, one stream shared by every test
// ===========================================================================
class BitIoRoundTripTest : public ::testing::Test {
protected:
    static constexpr int N = 50000;

    static std::vector<Pair> pairs;
    static std::vector<uint64_t> starts;   // bit position where each pair begins
    static Bytes buf;
    static uint64_t total_bits;
    static uint64_t writer_bits;

    static void SetUpTestSuite() {
        std::mt19937_64 rng(20260916);
        pairs = randomPairs(rng, N);

        starts.clear();
        total_bits = 0;
        for (const Pair& p : pairs) {
            starts.push_back(total_bits);
            total_bits += static_cast<uint64_t>(p.width);
        }

        buf.clear();
        BitWriter w(buf);
        for (const Pair& p : pairs) w.write_bits(p.raw, p.width);
        writer_bits = w.bits_written();
        w.flush();
    }
};

std::vector<Pair> BitIoRoundTripTest::pairs;
std::vector<uint64_t> BitIoRoundTripTest::starts;
Bytes BitIoRoundTripTest::buf;
uint64_t BitIoRoundTripTest::total_bits = 0;
uint64_t BitIoRoundTripTest::writer_bits = 0;

TEST_F(BitIoRoundTripTest, WriterCountedEveryBitItWasHanded) {
    EXPECT_EQ(writer_bits, total_bits);
}

TEST_F(BitIoRoundTripTest, BufferIsExactlyCeilBitsOverEightBytes) {
    EXPECT_EQ(buf.size(), (total_bits + 7) / 8);
}

TEST_F(BitIoRoundTripTest, EveryPairSurvivesTheRoundTrip) {
    BitReader r(buf);
    for (int i = 0; i < N; ++i) {
        ASSERT_EQ(r.read_bits(pairs[i].width), pairs[i].value)
            << "pair " << i << " width " << pairs[i].width;
    }
    EXPECT_TRUE(r.ok());
}

TEST_F(BitIoRoundTripTest, EveryPairStartsWhereTheCountersSayItDoes) {
    BitReader r(buf);
    for (int i = 0; i < N; ++i) {
        ASSERT_EQ(r.bits_read(), starts[i]) << "pair " << i;
        r.skip_bits(pairs[i].width);
    }
}

TEST_F(BitIoRoundTripTest, PeekAtWidthAgreesWithReadAtEveryPosition) {
    BitReader r(buf);
    for (int i = 0; i < N; ++i) {
        const uint64_t peeked = r.peek_bits(pairs[i].width);
        ASSERT_EQ(peeked, pairs[i].value) << "pair " << i;
        ASSERT_EQ(r.read_bits(pairs[i].width), peeked) << "pair " << i;
    }
}

TEST_F(BitIoRoundTripTest, SixtyFourBitPeekWindowHoldsTheNextCodeInItsTopBits) {
    // The table decoder's view of the stream: a wide window, the code at the top.
    BitReader r(buf);
    for (int i = 0; i < N; ++i) {
        const uint64_t window = r.peek_bits(64);
        ASSERT_EQ(window >> (64 - pairs[i].width), pairs[i].value) << "pair " << i;
        r.skip_bits(pairs[i].width);
    }
    EXPECT_TRUE(r.ok()) << "a 64-bit peek overhanging the end must not flag overrun";
}

TEST_F(BitIoRoundTripTest, PeekNeverAdvancesTheReader) {
    BitReader r(buf);
    for (int i = 0; i < N; i += 97) {
        while (r.bits_read() < starts[i]) {
            const uint64_t gap = starts[i] - r.bits_read();
            r.skip_bits(gap > 64 ? 64 : static_cast<int>(gap));
        }
        const uint64_t before = r.bits_read();
        for (int k = 0; k < 5; ++k) r.peek_bits(pairs[i].width);
        ASSERT_EQ(r.bits_read(), before);
        ASSERT_EQ(r.read_bits(pairs[i].width), pairs[i].value);
    }
}

TEST_F(BitIoRoundTripTest, ReaderConsumesExactlyWhatTheWriterProducedAndPadIsZero) {
    BitReader r(buf);
    for (const Pair& p : pairs) r.skip_bits(p.width);
    EXPECT_EQ(r.bits_read(), total_bits);
    EXPECT_EQ(r.bits_remaining(), (8 - total_bits % 8) % 8);
    EXPECT_EQ(r.read_bits(static_cast<int>(r.bits_remaining())), 0u) << "padding must be zero";
    EXPECT_TRUE(r.exhausted());
    EXPECT_TRUE(r.ok());
}

TEST_F(BitIoRoundTripTest, BitByBitReadRebuildsTheSameValues) {
    BitReader r(buf);
    for (int i = 0; i < N; i += 7) {
        while (r.bits_read() < starts[i]) {
            const uint64_t gap = starts[i] - r.bits_read();
            r.skip_bits(gap > 64 ? 64 : static_cast<int>(gap));
        }
        uint64_t v = 0;
        for (int b = 0; b < pairs[i].width; ++b) v = (v << 1) | (r.read_bit() ? 1 : 0);
        ASSERT_EQ(v, pairs[i].value) << "pair " << i;
    }
}

TEST_F(BitIoRoundTripTest, ReadingOneBitTooManyAtTheEndOverruns) {
    BitReader r(buf);
    while (!r.exhausted()) r.skip_bits(r.bits_remaining() > 64 ? 64 : static_cast<int>(r.bits_remaining()));
    EXPECT_TRUE(r.ok());
    r.read_bit();
    EXPECT_TRUE(r.overrun());
}

// ===========================================================================
// Edge cases
// ===========================================================================
TEST(BitIoEdgeTest, EmptyStream) {
    Bytes buf;
    {
        BitWriter w(buf);
        EXPECT_EQ(w.bits_written(), 0u);
    }
    EXPECT_TRUE(buf.empty());
    BitReader r(buf);
    EXPECT_TRUE(r.exhausted());
    EXPECT_EQ(r.peek_bits(15), 0u);
    EXPECT_TRUE(r.ok());
}

TEST(BitIoEdgeTest, ThreeBitsAreLeftAlignedAndPadIsZero) {
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(0b101, 3);
    }
    ASSERT_EQ(buf, (Bytes{0xA0}));
    BitReader r(buf);
    EXPECT_EQ(r.read_bits(3), 0b101u);
    EXPECT_EQ(r.read_bits(5), 0u);
    EXPECT_TRUE(r.ok());
}

TEST(BitIoEdgeTest, FullSixtyFourBitValues) {
    for (uint64_t v : {0ull, 1ull, ~0ull, 0x8000000000000000ull, 0x0123456789ABCDEFull, 0xAAAAAAAAAAAAAAAAull}) {
        Bytes buf;
        {
            BitWriter w(buf);
            w.write_bits(v, 64);
        }
        BitReader r(buf);
        EXPECT_EQ(r.read_bits(64), v);
        EXPECT_TRUE(r.exhausted());
    }
}

TEST(BitIoEdgeTest, WidthZeroWritesAndReadsNothing) {
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(~0ull, 0);
        w.write_bits(0b1, 1);
        w.write_bits(~0ull, 0);
    }
    BitReader r(buf);
    EXPECT_EQ(r.read_bits(0), 0u);
    EXPECT_EQ(r.read_bits(1), 1u);
    EXPECT_EQ(r.read_bits(0), 0u);
    EXPECT_EQ(r.bits_read(), 1u);
}

TEST(BitIoEdgeTest, ClampedWidthsAgreeOnBothSides) {
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(0xFEDCBA9876543210ull, 100);   // written as 64
        w.write_bits(0x5, -3);                      // nothing
        w.write_bits(0x5, 3);
    }
    BitReader r(buf);
    EXPECT_EQ(r.read_bits(100), 0xFEDCBA9876543210ull);
    EXPECT_EQ(r.read_bits(-3), 0u);
    EXPECT_EQ(r.read_bits(3), 0x5u);
    EXPECT_TRUE(r.ok());
}

TEST(BitIoEdgeTest, SkipJumpsAWholeSymbol) {
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(0x1234, 13);
        w.write_bits(0x2A, 6);
    }
    BitReader r(buf);
    r.skip_bits(13);
    EXPECT_EQ(r.read_bits(6), 0x2Au);
}

TEST(BitIoEdgeTest, MidStreamFlushIsMirroredByAlignToByte) {
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(0b11, 2);
        w.flush();
        w.write_bits(0x7F, 7);
        w.flush();
        w.write_bits(0x1, 1);
    }
    ASSERT_EQ(buf, (Bytes{0xC0, 0xFE, 0x80}));
    BitReader r(buf);
    EXPECT_EQ(r.read_bits(2), 0b11u);
    r.align_to_byte();
    EXPECT_EQ(r.read_bits(7), 0x7Fu);
    r.align_to_byte();
    EXPECT_EQ(r.read_bits(1), 1u);
    r.align_to_byte();
    EXPECT_TRUE(r.exhausted());
    EXPECT_TRUE(r.ok());
}

TEST(BitIoEdgeTest, WriterAndReaderCountersAgreeAtEverySymbolBoundary) {
    std::mt19937_64 rng(11);
    const std::vector<Pair> pairs = randomPairs(rng, 2000);
    std::vector<uint64_t> writerCounts;
    Bytes buf;
    {
        BitWriter w(buf);
        for (const Pair& p : pairs) {
            writerCounts.push_back(w.bits_written());
            w.write_bits(p.raw, p.width);
        }
    }
    BitReader r(buf);
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        ASSERT_EQ(r.bits_read(), writerCounts[i]) << "symbol " << i;
        ASSERT_EQ(r.read_bits(pairs[i].width), pairs[i].value);
    }
}

TEST(BitIoEdgeTest, HuffmanSizedCodesOfOneToFifteenBits) {
    // The widths a canonical code really uses, 300,000 of them.
    std::mt19937_64 rng(12);
    const std::vector<Pair> pairs = randomPairs(rng, 300000, 1, 15);
    Bytes buf;
    {
        BitWriter w(buf);
        for (const Pair& p : pairs) w.write_bits(p.value, p.width);
    }
    BitReader r(buf);
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        const uint64_t window = r.peek_bits(15);
        ASSERT_EQ(window >> (15 - pairs[i].width), pairs[i].value) << "symbol " << i;
        r.skip_bits(pairs[i].width);
    }
    EXPECT_TRUE(r.ok());
}

// ===========================================================================
// Every width 1..64: structured and random values back to back
// ===========================================================================
class BitIoWidthTest : public ::testing::TestWithParam<int> {};

TEST_P(BitIoWidthTest, StructuredAndRandomValuesRoundTrip) {
    const int width = GetParam();
    std::vector<uint64_t> values = {
        0, 1, maskTo(~0ull, width), 1ull << (width - 1),
        maskTo(0xAAAAAAAAAAAAAAAAull, width), maskTo(0x5555555555555555ull, width),
        maskTo(0x0123456789ABCDEFull, width),
    };
    std::mt19937_64 rng(300 + width);
    for (int i = 0; i < 1000; ++i) values.push_back(maskTo(rng(), width));

    Bytes buf;
    {
        BitWriter w(buf);
        for (uint64_t v : values) w.write_bits(v, width);
    }
    EXPECT_EQ(buf.size(), (values.size() * width + 7) / 8);
    BitReader r(buf);
    for (std::size_t i = 0; i < values.size(); ++i) ASSERT_EQ(r.read_bits(width), values[i]) << "value " << i;
    EXPECT_TRUE(r.ok());
}

TEST_P(BitIoWidthTest, ValueFollowedByAMarkerBitStaysAligned) {
    const int width = GetParam();
    std::mt19937_64 rng(400 + width);
    std::vector<uint64_t> values;
    Bytes buf;
    {
        BitWriter w(buf);
        for (int i = 0; i < 300; ++i) {
            values.push_back(maskTo(rng(), width));
            w.write_bits(values.back(), width);
            w.write_bit(true);
        }
    }
    BitReader r(buf);
    for (std::size_t i = 0; i < values.size(); ++i) {
        ASSERT_EQ(r.read_bits(width), values[i]) << i;
        ASSERT_TRUE(r.read_bit()) << "marker after value " << i;
    }
}

TEST_P(BitIoWidthTest, ByteAlignedSegmentsWithFlushAndAlign) {
    const int width = GetParam();
    std::mt19937_64 rng(500 + width);
    std::vector<uint64_t> values;
    Bytes buf;
    {
        BitWriter w(buf);
        for (int i = 0; i < 200; ++i) {
            values.push_back(maskTo(rng(), width));
            w.write_bits(values.back(), width);
            w.flush();
        }
    }
    EXPECT_EQ(buf.size(), values.size() * ((width + 7) / 8));
    BitReader r(buf);
    for (std::size_t i = 0; i < values.size(); ++i) {
        ASSERT_EQ(r.read_bits(width), values[i]) << i;
        r.align_to_byte();
    }
    EXPECT_TRUE(r.exhausted());
    EXPECT_TRUE(r.ok());
}

INSTANTIATE_TEST_SUITE_P(AllWidths, BitIoWidthTest, ::testing::Range(1, 65));

// ===========================================================================
// Random streams: 150 seeds, each a random mix of bits, words and flushes
// ===========================================================================
class BitIoRandomStreamTest : public ::testing::TestWithParam<int> {};

TEST_P(BitIoRandomStreamTest, MixedOperationsReadBackInTheSameOrder) {
    std::mt19937_64 rng(static_cast<uint64_t>(GetParam()) * 31337 + 9);

    enum Kind { Bit, Word, Flush };
    struct Op { Kind kind; uint64_t value; int width; };
    std::vector<Op> ops;

    Bytes buf;
    uint64_t expectedBits = 0;
    {
        BitWriter w(buf);
        const int n = 200 + static_cast<int>(rng() % 800);
        for (int i = 0; i < n; ++i) {
            const int pick = static_cast<int>(rng() % 10);
            if (pick < 3) {
                const bool b = (rng() & 1) != 0;
                w.write_bit(b);
                ops.push_back({Bit, b ? 1u : 0u, 1});
                ++expectedBits;
            } else if (pick < 9) {
                const int width = 1 + static_cast<int>(rng() % 64);
                const uint64_t raw = rng();
                w.write_bits(raw, width);
                ops.push_back({Word, maskTo(raw, width), width});
                expectedBits += width;
            } else {
                w.flush();
                ops.push_back({Flush, 0, 0});
            }
        }
        ASSERT_EQ(w.bits_written(), expectedBits);
    }

    BitReader r(buf);
    for (std::size_t i = 0; i < ops.size(); ++i) {
        switch (ops[i].kind) {
            case Bit:   ASSERT_EQ(r.read_bit() ? 1u : 0u, ops[i].value) << "op " << i; break;
            case Word:  ASSERT_EQ(r.read_bits(ops[i].width), ops[i].value) << "op " << i; break;
            case Flush: r.align_to_byte(); break;
        }
    }
    r.align_to_byte();
    EXPECT_TRUE(r.exhausted());
    EXPECT_TRUE(r.ok());
}

INSTANTIATE_TEST_SUITE_P(Seeds, BitIoRandomStreamTest, ::testing::Range(0, 150));
