// test_bitio.cpp -- BitWriter -> BitReader round trips.
// Not a single line of Huffman code is involved: if this passes, bit I/O is off
// the suspect list for good.
#include "BitReader.hpp"
#include "BitWriter.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <vector>

using Bytes = std::vector<uint8_t>;

namespace {

// Low `width` bits of `v`. width is 1..64, so the 1ull << 64 trap is avoided.
uint64_t mask_to(uint64_t v, int width) {
    return width >= 64 ? v : v & ((1ull << width) - 1);
}

struct Pair {
    uint64_t raw;      // what gets handed to write_bits, high junk included
    uint64_t value;    // what must come back out
    int width;
};

}  // namespace

// ---------------------------------------------------------------------------
// The main event: 50,000 random (value, width) pairs, fixed seed. The stream is
// built once and each property gets its own test.
// ---------------------------------------------------------------------------
class BitIoRoundTripTest : public ::testing::Test {
protected:
    static constexpr int N = 50000;

    static std::vector<Pair> pairs;
    static Bytes buf;
    static uint64_t total_bits;
    static uint64_t writer_bits;

    static void SetUpTestSuite() {
        std::mt19937_64 rng(20260916);   // fixed seed -> byte-identical every run

        pairs.clear();
        pairs.reserve(N);
        total_bits = 0;
        for (int i = 0; i < N; ++i) {
            int width = static_cast<int>(rng() % 64) + 1;   // 1..64
            uint64_t raw = rng();                           // junk above `width` too
            pairs.push_back({raw, mask_to(raw, width), width});
            total_bits += static_cast<uint64_t>(width);
        }

        buf.clear();
        BitWriter w(buf);
        for (const Pair& p : pairs) w.write_bits(p.raw, p.width);
        writer_bits = w.bits_written();
        w.flush();
    }
};

std::vector<Pair> BitIoRoundTripTest::pairs;
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
    int mismatches = 0;
    for (int i = 0; i < N; ++i) {
        const Pair& p = pairs[i];
        uint64_t got = r.read_bits(p.width);
        if (got != p.value && mismatches++ < 5) {
            ADD_FAILURE() << "symbol #" << i << " width " << p.width << ": want 0x"
                          << std::hex << p.value << ", got 0x" << got;
        }
    }
    EXPECT_EQ(mismatches, 0);
}

TEST_F(BitIoRoundTripTest, PeekAtWidthAgreesWithReadAtEveryPosition) {
    BitReader r(buf);
    int mismatches = 0;
    for (const Pair& p : pairs) {
        uint64_t peeked = r.peek_bits(p.width);
        if (peeked != r.read_bits(p.width)) ++mismatches;
    }
    EXPECT_EQ(mismatches, 0);
}

TEST_F(BitIoRoundTripTest, SixtyFourBitPeekWindowHoldsTheNextCodeInItsTopBits) {
    // How a table decoder looks at the stream: a fixed-width window.
    BitReader r(buf);
    int mismatches = 0;
    for (const Pair& p : pairs) {
        uint64_t window = r.peek_bits(64) >> (64 - p.width);
        if (window != p.value) ++mismatches;
        r.skip_bits(p.width);
    }
    EXPECT_EQ(mismatches, 0);
    EXPECT_TRUE(r.ok());
}

TEST_F(BitIoRoundTripTest, PeekNeverAdvancesTheReader) {
    BitReader r(buf);
    int advances = 0;
    for (const Pair& p : pairs) {
        uint64_t before = r.bits_read();
        r.peek_bits(p.width);
        r.peek_bits(64);
        if (r.bits_read() != before) ++advances;
        r.read_bits(p.width);
    }
    EXPECT_EQ(advances, 0);
}

TEST_F(BitIoRoundTripTest, ReaderConsumesExactlyWhatTheWriterProducedAndPadIsUntouched) {
    BitReader r(buf);
    for (const Pair& p : pairs) r.read_bits(p.width);

    EXPECT_EQ(r.bits_read(), total_bits);
    EXPECT_TRUE(r.ok()) << "the trailing pad bits must never be needed";

    // The framing decision in practice: the header would carry N, the decoder
    // stops after N symbols, and the leftover pad bits are reachable but irrelevant.
    EXPECT_EQ(r.bits_remaining(), buf.size() * 8 - total_bits);
    EXPECT_LT(r.bits_remaining(), 8u);
}

TEST_F(BitIoRoundTripTest, BitByBitReadRebuildsTheSameValues) {
    BitReader r(buf);
    int mismatches = 0;
    for (int i = 0; i < 2000; ++i) {   // a prefix is plenty; this path is slow
        const Pair& p = pairs[i];
        uint64_t v = 0;
        for (int b = 0; b < p.width; ++b) v = (v << 1) | (r.read_bit() ? 1u : 0u);
        if (v != p.value) ++mismatches;
    }
    EXPECT_EQ(mismatches, 0);
}

// ---------------------------------------------------------------------------
// Edge cases around the seams.
// ---------------------------------------------------------------------------
TEST(BitIoEdgeTest, EmptyStream) {
    Bytes buf;
    { BitWriter w(buf); }
    BitReader r(buf);
    EXPECT_TRUE(r.exhausted());
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.peek_bits(64), 0u);
    EXPECT_TRUE(r.ok()) << "peek on an empty stream stays clean";
    r.read_bits(1);
    EXPECT_FALSE(r.ok()) << "reading an empty stream sets the overrun flag";
}

TEST(BitIoEdgeTest, ThreeBitsAreLeftAlignedAndPadIsZero) {
    Bytes buf;
    { BitWriter w(buf); w.write_bits(0b101, 3); }
    EXPECT_EQ(buf, Bytes{0xA0});

    BitReader r(buf);
    EXPECT_EQ(r.read_bits(3), 0b101u);
    EXPECT_EQ(r.bits_remaining(), 5u);
    EXPECT_EQ(r.read_bits(5), 0u);
    EXPECT_TRUE(r.ok());
}

TEST(BitIoEdgeTest, FullSixtyFourBitValue) {
    Bytes buf;
    { BitWriter w(buf); w.write_bits(0xDEADBEEFCAFEBABEull, 64); }
    BitReader r(buf);
    EXPECT_EQ(r.read_bits(64), 0xDEADBEEFCAFEBABEull);
    EXPECT_TRUE(r.ok());
}

TEST(BitIoEdgeTest, WidthZeroWritesAndReadsNothing) {
    Bytes buf;
    { BitWriter w(buf); w.write_bits(0xFF, 0); w.write_bit(true); }
    EXPECT_EQ(buf, Bytes{0x80});

    BitReader r(buf);
    EXPECT_EQ(r.read_bits(0), 0u);
    EXPECT_EQ(r.bits_read(), 0u);
    EXPECT_TRUE(r.read_bit());
}

TEST(BitIoEdgeTest, ClampedWidthsAgreeOnBothSides) {
    Bytes buf;
    { BitWriter w(buf); w.write_bits(0x0123456789ABCDEFull, 99); w.write_bits(0b11, 2); }
    BitReader r(buf);
    EXPECT_EQ(r.read_bits(99), 0x0123456789ABCDEFull);
    EXPECT_EQ(r.read_bits(2), 0b11u);
    EXPECT_TRUE(r.ok());
}

TEST(BitIoEdgeTest, SkipJumpsAWholeSymbol) {
    Bytes buf;
    { BitWriter w(buf); w.write_bits(0xAB, 8); w.write_bits(0xCD, 8); }
    BitReader r(buf);
    r.skip_bits(8);
    EXPECT_EQ(r.read_bits(8), 0xCDu);
}

TEST(BitIoEdgeTest, MidStreamFlushIsMirroredByAlignToByte) {
    Bytes buf;
    {
        BitWriter w(buf);
        w.write_bits(0b111, 3);
        w.flush();                  // writer pads to the byte boundary
        w.write_bits(0xAA, 8);
    }
    BitReader r(buf);
    EXPECT_EQ(r.read_bits(3), 0b111u);
    r.align_to_byte();
    EXPECT_EQ(r.read_bits(8), 0xAAu);
    r.align_to_byte();
    EXPECT_EQ(r.bits_read(), 16u) << "align_to_byte is a no-op when already aligned";
}

TEST(BitIoEdgeTest, ManyFlushAlignSegments) {
    std::mt19937 rng(7);
    std::vector<std::vector<Pair>> segments(200);

    Bytes buf;
    {
        BitWriter w(buf);
        for (auto& seg : segments) {
            int count = static_cast<int>(rng() % 6);   // empty segments included
            for (int i = 0; i < count; ++i) {
                int width = static_cast<int>(rng() % 20) + 1;
                uint64_t raw = (static_cast<uint64_t>(rng()) << 32) | rng();
                seg.push_back({raw, mask_to(raw, width), width});
                w.write_bits(raw, width);
            }
            w.flush();
            EXPECT_EQ(w.bits_pending(), 0);
        }
    }

    BitReader r(buf);
    for (std::size_t s = 0; s < segments.size(); ++s) {
        for (const Pair& p : segments[s]) {
            ASSERT_EQ(r.read_bits(p.width), p.value) << "segment " << s;
        }
        r.align_to_byte();
    }
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(r.exhausted());
}

TEST(BitIoEdgeTest, OverLongReadZeroFillsAndFlags) {
    Bytes buf;
    { BitWriter w(buf); w.write_bits(0b1011, 4); }   // 4 real bits + 4 pad
    BitReader r(buf);
    EXPECT_EQ(r.read_bits(16), 0b10110000ull << 8);
    EXPECT_FALSE(r.ok());
}

TEST(BitIoEdgeTest, PeekPastTheEndDoesNotFlagOverrun) {
    // The table decoder depends on this.
    Bytes buf;
    { BitWriter w(buf); w.write_bits(0b1, 1); }
    BitReader r(buf);
    EXPECT_EQ(r.peek_bits(64), 0b10000000ull << 56);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.peek_bits(64), r.peek_bits(64));
    EXPECT_TRUE(r.ok());
}

TEST(BitIoEdgeTest, WriterAndReaderCountersAgreeAtEverySymbolBoundary) {
    std::mt19937_64 rng(99);
    std::vector<Pair> pairs;
    std::vector<uint64_t> boundaries;

    Bytes buf;
    {
        BitWriter w(buf);
        for (int i = 0; i < 1000; ++i) {
            int width = static_cast<int>(rng() % 64) + 1;
            uint64_t raw = rng();
            pairs.push_back({raw, mask_to(raw, width), width});
            w.write_bits(raw, width);
            boundaries.push_back(w.bits_written());
        }
    }

    BitReader r(buf);
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        r.read_bits(pairs[i].width);
        ASSERT_EQ(r.bits_read(), boundaries[i]) << "symbol " << i;
    }
}

// ---------------------------------------------------------------------------
// Every width 1..64 with structured and random values.
// ---------------------------------------------------------------------------
class BitIoWidthTest : public ::testing::TestWithParam<int> {};

TEST_P(BitIoWidthTest, StructuredAndRandomValuesRoundTrip) {
    const int width = GetParam();
    std::mt19937_64 rng(1000 + width);

    std::vector<uint64_t> values = {
        0, ~0ull, 0xAAAAAAAAAAAAAAAAull, 0x5555555555555555ull, 1, 1ull << (width - 1),
    };
    for (int i = 0; i < 100; ++i) values.push_back(rng());

    Bytes buf;
    uint64_t written = 0;
    {
        BitWriter w(buf);
        for (uint64_t v : values) w.write_bits(v, width);
        written = w.bits_written();
    }
    const uint64_t total = static_cast<uint64_t>(width) * values.size();
    ASSERT_EQ(written, total);
    ASSERT_EQ(buf.size(), (total + 7) / 8);

    BitReader r(buf);
    for (std::size_t i = 0; i < values.size(); ++i) {
        EXPECT_EQ(r.peek_bits(width), mask_to(values[i], width)) << "value " << i;
        EXPECT_EQ(r.read_bits(width), mask_to(values[i], width)) << "value " << i;
    }
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.bits_remaining(), buf.size() * 8 - total);
    EXPECT_EQ(r.read_bits(static_cast<int>(r.bits_remaining())), 0u) << "pad bits are zero";
    EXPECT_TRUE(r.ok());
}

TEST_P(BitIoWidthTest, ValueFollowedByMarkerBitStaysAligned) {
    // A single stray or missing bit anywhere would shift every marker.
    const int width = GetParam();
    std::mt19937_64 rng(2000 + width);
    std::vector<uint64_t> values;
    for (int i = 0; i < 50; ++i) values.push_back(rng());

    Bytes buf;
    {
        BitWriter w(buf);
        for (std::size_t i = 0; i < values.size(); ++i) {
            w.write_bits(values[i], width);
            w.write_bit(i % 2 == 0);
        }
    }

    BitReader r(buf);
    for (std::size_t i = 0; i < values.size(); ++i) {
        ASSERT_EQ(r.read_bits(width), mask_to(values[i], width)) << "value " << i;
        ASSERT_EQ(r.read_bit(), i % 2 == 0) << "marker " << i;
    }
    EXPECT_TRUE(r.ok());
}

INSTANTIATE_TEST_SUITE_P(AllWidths, BitIoWidthTest, ::testing::Range(1, 65));
