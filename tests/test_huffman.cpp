// test_huffman.cpp -- hf:: from frequency counting to canonical codes, plus an
// encode (BitWriter) -> decode (BitReader) round trip over every data shape.
//
// Hand-checked vectors (CLRS 16.3, RFC 1951 3.2.2, "abracadabra") pin exact
// output. The parameterized suites check, for thousands of seeded tables, the
// properties any correct build must have: optimal cost against an independent
// min-heap Huffman, a complete Kraft sum, the 15-bit cap, order preservation
// in the limiter, and codes that follow the canonical rule exactly.
#include "core/BitReader.hpp"
#include "core/BitWriter.hpp"
#include "core/Huffman.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <numeric>
#include <queue>
#include <random>
#include <string>
#include <utility>
#include <vector>

using hf::CodeLengths;
using hf::CodeTable;
using hf::FrequencyTable;
using ts::Bytes;
using ts::Shape;

namespace {

// ---------------------------------------------------------------------------
// Independent references
// ---------------------------------------------------------------------------

// Optimal prefix-code cost from a textbook min-heap Huffman: the sum of all
// merge weights. Shares nothing with the two-queue implementation.
uint64_t referenceHuffmanCost(const FrequencyTable& freq) {
    std::priority_queue<uint64_t, std::vector<uint64_t>, std::greater<uint64_t>> pq;
    for (uint64_t f : freq)
        if (f != 0) pq.push(f);
    if (pq.size() == 1) return pq.top();   // one symbol still spends one bit per byte
    uint64_t total = 0;
    while (pq.size() > 1) {
        const uint64_t a = pq.top(); pq.pop();
        const uint64_t b = pq.top(); pq.pop();
        total += a + b;
        pq.push(a + b);
    }
    return total;
}

uint64_t cost(const FrequencyTable& freq, const CodeLengths& len) {
    uint64_t c = 0;
    for (int s = 0; s < 256; ++s) c += freq[s] * len[s];
    return c;
}

int maxLen(const CodeLengths& len) { return *std::max_element(len.begin(), len.end()); }

int activeSymbols(const CodeLengths& len) {
    return static_cast<int>(std::count_if(len.begin(), len.end(), [](uint8_t l) { return l != 0; }));
}

// Exact Kraft comparisons for any length up to 255, by folding the tree up
// from its deepest level. ceil() folding gives ceil(sum), exact folding needs
// an even count at every level.
bool kraftComplete(const CodeLengths& len) {
    std::array<uint64_t, 256> count{};
    for (uint8_t l : len)
        if (l) ++count[l];
    uint64_t nodes = 0;
    for (int l = 255; l >= 1; --l) {
        nodes += count[l];
        if (nodes % 2) return false;
        nodes /= 2;
    }
    return nodes == 1;
}

bool kraftAtMostOne(const CodeLengths& len) {
    std::array<uint64_t, 256> count{};
    for (uint8_t l : len)
        if (l) ++count[l];
    uint64_t nodes = 0;
    for (int l = 255; l >= 1; --l) nodes = (nodes + count[l] + 1) / 2;
    return nodes <= 1;
}

uint64_t kraftSum15(const CodeLengths& len) {
    uint64_t k = 0;
    for (uint8_t l : len)
        if (l != 0 && l <= 15) k += 1ull << (15 - l);
    return k;
}

bool isPrefixFree(const CodeLengths& len, const CodeTable& codes) {
    for (int a = 0; a < 256; ++a) {
        if (!len[a]) continue;
        for (int b = 0; b < 256; ++b) {
            if (a == b || !len[b] || len[a] > len[b]) continue;
            if ((codes[b] >> (len[b] - len[a])) == codes[a]) return false;   // a is a prefix of b
        }
    }
    return true;
}

// The canonical rule from scratch: walk the symbols by (length, value), the
// first gets 0, each next one is the previous + 1 shifted left by the length step.
CodeTable referenceCanonical(const CodeLengths& len) {
    std::vector<int> order;
    for (int s = 0; s < 256; ++s)
        if (len[s]) order.push_back(s);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return len[a] < len[b]; });

    CodeTable codes{};
    uint32_t code = 0;   // the first code is all zeros, whatever its length
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (i > 0) code = (code + 1) << (len[order[i]] - len[order[i - 1]]);
        codes[order[i]] = code;
    }
    return codes;
}

CodeLengths lengthsOf(std::initializer_list<std::pair<int, int>> symbolLength) {
    CodeLengths len{};
    for (const auto& sl : symbolLength) len[sl.first] = static_cast<uint8_t>(sl.second);
    return len;
}

// Symbol k gets weight F(k+1): 1, 1, 2, 3, 5, 8, ... n symbols make a tree
// n-1 deep. n <= 90 keeps the total inside uint64_t.
FrequencyTable fibFreq(int n) {
    FrequencyTable freq{};
    uint64_t a = 1, b = 1;
    for (int k = 0; k < n; ++k) {
        freq[k] = a;
        const uint64_t next = a + b;
        a = b;
        b = next;
    }
    return freq;
}

enum class Dist { Uniform, Sparse, Skewed, Extreme };

const char* distName(Dist d) {
    switch (d) {
        case Dist::Uniform: return "Uniform";
        case Dist::Sparse:  return "Sparse";
        case Dist::Skewed:  return "Skewed";
        case Dist::Extreme: return "Extreme";
    }
    return "?";
}

FrequencyTable randomFrequencies(std::mt19937_64& rng, Dist dist) {
    std::array<int, 256> symbols;
    std::iota(symbols.begin(), symbols.end(), 0);
    std::shuffle(symbols.begin(), symbols.end(), rng);

    FrequencyTable freq{};
    int k = 0;
    switch (dist) {
        case Dist::Uniform:   // many symbols, flat-ish counts
            k = 2 + static_cast<int>(rng() % 255);
            for (int i = 0; i < k; ++i) freq[symbols[i]] = 1 + rng() % 1000;
            break;
        case Dist::Sparse:    // a handful of symbols, wide counts
            k = 2 + static_cast<int>(rng() % 7);
            for (int i = 0; i < k; ++i) freq[symbols[i]] = 1 + rng() % 1000000;
            break;
        case Dist::Skewed:    // counts spread over 40 powers of two: deep trees
            k = 16 + static_cast<int>(rng() % 241);
            for (int i = 0; i < k; ++i) freq[symbols[i]] = (1ull << (rng() % 40)) + rng() % 16;
            break;
        case Dist::Extreme:   // up to 2^55 per symbol, so even 256 of them sum below 2^64
            k = 2 + static_cast<int>(rng() % 255);
            for (int i = 0; i < k; ++i) freq[symbols[i]] = 1 + (rng() >> (9 + rng() % 55));
            break;
    }
    return freq;
}

// A random complete binary tree with n leaves, grown by splitting leaves;
// `deep` picks the deepest leaf most of the time, which drives depth towards
// n-1. The leaf depths are handed to random symbols.
CodeLengths randomTreeLengths(std::mt19937_64& rng, int n, bool deep) {
    std::vector<int> depths = {0};
    while (static_cast<int>(depths.size()) < n) {
        std::size_t pick;
        if (deep && rng() % 4 != 0) {
            pick = static_cast<std::size_t>(std::max_element(depths.begin(), depths.end()) - depths.begin());
        } else {
            pick = rng() % depths.size();
        }
        const int d = depths[pick] + 1;
        depths[pick] = d;
        depths.push_back(d);
    }
    if (n == 1) depths[0] = 1;

    std::array<int, 256> symbols;
    std::iota(symbols.begin(), symbols.end(), 0);
    std::shuffle(symbols.begin(), symbols.end(), rng);
    CodeLengths len{};
    for (int i = 0; i < n; ++i) len[symbols[i]] = static_cast<uint8_t>(std::min(depths[i], 255));
    return len;
}

// ---------------------------------------------------------------------------
// Encode / decode through the bit I/O, with two decoders that share nothing
// ---------------------------------------------------------------------------
struct Encoded {
    FrequencyTable freq{};
    CodeLengths unlimited{};
    CodeLengths len{};
    CodeTable codes{};
    bool usable = false;
    Bytes stream;
    uint64_t bits_written = 0;
};

Encoded encode(const Bytes& data) {
    Encoded e;
    e.freq = hf::countFrequencies(data);
    e.unlimited = hf::buildCodeLengths(e.freq);
    e.len = hf::LimitCodeLengths(e.unlimited);
    e.usable = hf::buildCanonicalCodes(e.len, e.codes);
    {
        BitWriter w(e.stream);
        for (uint8_t b : data) w.write_bits(e.codes[b], e.len[b]);
        e.bits_written = w.bits_written();
    }
    return e;
}

struct Decoded {
    Bytes out;
    uint64_t bits_read = 0;
    bool ok = false;
    bool bad_code = false;
};

// One bit at a time, looking the growing code up in a map built from the
// code table. Knows nothing about how canonical codes are assigned.
Decoded decodeBitByBit(const Encoded& e, std::size_t symbols) {
    std::map<std::pair<int, uint32_t>, uint8_t> lookup;
    for (int s = 0; s < 256; ++s)
        if (e.len[s] != 0) lookup[{e.len[s], e.codes[s]}] = static_cast<uint8_t>(s);

    Decoded d;
    BitReader r(e.stream);
    for (std::size_t i = 0; i < symbols && !d.bad_code; ++i) {
        uint32_t code = 0;
        bool found = false;
        for (int l = 1; l <= hf::MAX_CODE_LEN && !found; ++l) {
            code = (code << 1) | (r.read_bit() ? 1u : 0u);
            const auto it = lookup.find({l, code});
            if (it != lookup.end()) {
                d.out.push_back(it->second);
                found = true;
            }
        }
        d.bad_code = !found;
    }
    d.bits_read = r.bits_read();
    d.ok = r.ok();
    return d;
}

// A 2^15-entry table indexed by a fixed 15-bit peek, then a skip of the real
// code length. Near the end the window overhangs the data; that must NOT
// count as an overrun.
Decoded decodeWithTable(const Encoded& e, std::size_t symbols) {
    struct Entry { uint8_t sym; uint8_t len; };
    std::vector<Entry> table(1u << hf::MAX_CODE_LEN, Entry{0, 0});

    Decoded d;
    for (int s = 0; s < 256; ++s) {
        if (e.len[s] == 0) continue;
        const int shift = hf::MAX_CODE_LEN - e.len[s];
        const uint32_t first = e.codes[s] << shift;
        const uint32_t last = (e.codes[s] + 1) << shift;
        for (uint32_t idx = first; idx < last && idx < table.size(); ++idx) {
            if (table[idx].len != 0) d.bad_code = true;   // two codes claim one slot
            table[idx] = {static_cast<uint8_t>(s), e.len[s]};
        }
    }

    BitReader r(e.stream);
    for (std::size_t i = 0; i < symbols && !d.bad_code; ++i) {
        const Entry& entry = table[r.peek_bits(hf::MAX_CODE_LEN)];
        if (entry.len == 0) {
            d.bad_code = true;
            break;
        }
        r.skip_bits(entry.len);
        d.out.push_back(entry.sym);
    }
    d.bits_read = r.bits_read();
    d.ok = r.ok();
    return d;
}

void expectRoundTrip(const Bytes& data) {
    const Encoded e = encode(data);

    EXPECT_TRUE(e.usable);
    EXPECT_TRUE(hf::lengthsAreValid(e.len));
    EXPECT_LE(maxLen(e.len), hf::MAX_CODE_LEN);
    for (int s = 0; s < 256; ++s) EXPECT_EQ(e.len[s] != 0, e.freq[s] != 0) << "symbol " << s;
    EXPECT_TRUE(isPrefixFree(e.len, e.codes));
    EXPECT_EQ(e.bits_written, cost(e.freq, e.len));
    EXPECT_EQ(e.stream.size(), (e.bits_written + 7) / 8);

    const Decoded a = decodeBitByBit(e, data.size());
    EXPECT_FALSE(a.bad_code) << "bit-by-bit decoder hit an unknown code";
    EXPECT_TRUE(a.out == data) << "bit-by-bit decoder output differs from the input";
    EXPECT_EQ(a.bits_read, e.bits_written);
    EXPECT_TRUE(a.ok);

    const Decoded b = decodeWithTable(e, data.size());
    EXPECT_FALSE(b.bad_code) << "table decoder hit an empty or overlapping slot";
    EXPECT_TRUE(b.out == data) << "table decoder output differs from the input";
    EXPECT_EQ(b.bits_read, e.bits_written);
    EXPECT_TRUE(b.ok) << "a 15-bit peek overhanging the end must not flag overrun";
}

// Checks every property of a canonical code built from valid lengths.
void expectCanonical(const CodeLengths& len, const CodeTable& codes) {
    EXPECT_EQ(codes, referenceCanonical(len));
    EXPECT_TRUE(isPrefixFree(len, codes));
    for (int s = 0; s < 256; ++s) {
        if (len[s] == 0) {
            EXPECT_EQ(codes[s], 0u) << "unused symbol " << s;
        } else {
            EXPECT_LT(codes[s], 1u << len[s]) << "symbol " << s << " code wider than its length";
        }
    }
    // same length -> consecutive codes in symbol order
    for (int l = 1; l <= hf::MAX_CODE_LEN; ++l) {
        int prev = -1;
        for (int s = 0; s < 256; ++s) {
            if (len[s] != l) continue;
            if (prev >= 0) EXPECT_EQ(codes[s], codes[prev] + 1) << "length " << l << " symbols " << prev << "," << s;
            prev = s;
        }
    }
    // a complete code ends on all ones at its longest length
    if (activeSymbols(len) >= 2 && kraftComplete(len)) {
        const int m = maxLen(len);
        uint32_t last = 0;
        for (int s = 0; s < 256; ++s)
            if (len[s] == m) last = std::max(last, codes[s]);
        EXPECT_EQ(last, (1u << m) - 1);
    }
}

}  // namespace

// ===========================================================================
// countFrequencies
// ===========================================================================
TEST(CountFrequenciesTest, EmptyVectorGivesAllZeros) {
    EXPECT_EQ(hf::countFrequencies(Bytes{}), FrequencyTable{});
}

TEST(CountFrequenciesTest, NullPointerGivesAllZerosEvenWithNonZeroSize) {
    EXPECT_EQ(hf::countFrequencies(nullptr, 1000), FrequencyTable{});
    EXPECT_EQ(hf::countFrequencies(nullptr, 0), FrequencyTable{});
}

TEST(CountFrequenciesTest, ZeroSizeWithValidPointerGivesAllZeros) {
    const uint8_t data[] = {1, 2, 3};
    EXPECT_EQ(hf::countFrequencies(data, 0), FrequencyTable{});
}

TEST(CountFrequenciesTest, CountsBAB) {
    const FrequencyTable f = hf::countFrequencies(ts::bytesOf("BAB"));
    EXPECT_EQ(f['B'], 2u);
    EXPECT_EQ(f['A'], 1u);
    EXPECT_EQ(std::accumulate(f.begin(), f.end(), uint64_t{0}), 3u);
}

TEST(CountFrequenciesTest, OnlyTheFirstSizeBytesAreCounted) {
    const uint8_t data[] = {7, 7, 9, 9, 9};
    const FrequencyTable f = hf::countFrequencies(data, 3);
    EXPECT_EQ(f[7], 2u);
    EXPECT_EQ(f[9], 1u);
}

TEST(CountFrequenciesTest, EveryByteValueOnceGivesAllOnes) {
    Bytes data(256);
    std::iota(data.begin(), data.end(), 0);
    const FrequencyTable f = hf::countFrequencies(data);
    for (int s = 0; s < 256; ++s) EXPECT_EQ(f[s], 1u) << s;
}

TEST(CountFrequenciesTest, ExtremeByteValuesAreCounted) {
    const FrequencyTable f = hf::countFrequencies(Bytes{0x00, 0xFF, 0xFF, 0x00, 0x00});
    EXPECT_EQ(f[0x00], 3u);
    EXPECT_EQ(f[0xFF], 2u);
}

TEST(CountFrequenciesTest, LargeRunOfOneByte) {
    const Bytes data(1 << 20, 0x42);
    const FrequencyTable f = hf::countFrequencies(data);
    EXPECT_EQ(f[0x42], 1u << 20);
    EXPECT_EQ(std::accumulate(f.begin(), f.end(), uint64_t{0}), 1u << 20);
}

TEST(CountFrequenciesTest, VectorOverloadMatchesPointerOverload) {
    std::mt19937_64 rng(1);
    for (int trial = 0; trial < 50; ++trial) {
        const Bytes data = ts::randomBytes(rng, rng() % 5000);
        EXPECT_EQ(hf::countFrequencies(data), hf::countFrequencies(data.data(), data.size()));
    }
}

class CountFrequenciesShapeTest : public ::testing::TestWithParam<std::tuple<Shape, int>> {};

TEST_P(CountFrequenciesShapeTest, MatchesANaiveCountAndSumsToTheSize) {
    const Shape shape = std::get<0>(GetParam());
    const int size = std::get<1>(GetParam());
    const Bytes data = ts::makeData(shape, size, 1);
    const FrequencyTable f = hf::countFrequencies(data);
    std::map<int, uint64_t> naive;
    for (uint8_t b : data) ++naive[b];
    for (int s = 0; s < 256; ++s) ASSERT_EQ(f[s], naive.count(s) ? naive[s] : 0u) << "symbol " << s;
    EXPECT_EQ(std::accumulate(f.begin(), f.end(), uint64_t{0}), static_cast<uint64_t>(size));
}

INSTANTIATE_TEST_SUITE_P(ShapesAndSizes, CountFrequenciesShapeTest,
                         ::testing::Combine(::testing::ValuesIn(ts::allShapes()),
                                            ::testing::Values(0, 1, 2, 17, 256, 1000, 65536)),
                         [](const auto& info) {
                             return std::string(ts::shapeName(std::get<0>(info.param))) + "_" +
                                    std::to_string(std::get<1>(info.param));
                         });

// ===========================================================================
// buildCodeLengths
// ===========================================================================
TEST(BuildCodeLengthsTest, EmptyTableGivesAllZeros) {
    EXPECT_EQ(hf::buildCodeLengths(FrequencyTable{}), CodeLengths{});
}

TEST(BuildCodeLengthsTest, TwoSymbolsGetLengthOneEachWhateverTheirWeights) {
    std::mt19937_64 rng(2);
    for (int trial = 0; trial < 500; ++trial) {
        const int a = static_cast<int>(rng() % 256);
        int b = static_cast<int>(rng() % 256);
        if (b == a) b = (a + 1) % 256;
        FrequencyTable freq{};
        freq[a] = 1 + (rng() >> (2 + rng() % 62));   // 1 .. 2^62, the pair stays below 2^64
        freq[b] = 1 + (rng() >> (2 + rng() % 62));
        EXPECT_EQ(hf::buildCodeLengths(freq), lengthsOf({{a, 1}, {b, 1}})) << a << "," << b;
    }
}

TEST(BuildCodeLengthsTest, ClrsExample) {
    // CLRS 16.3: a:45 b:13 c:12 d:16 e:9 f:5 -> a=1, b=c=d=3, e=f=4, cost 224.
    FrequencyTable freq{};
    freq['a'] = 45; freq['b'] = 13; freq['c'] = 12;
    freq['d'] = 16; freq['e'] = 9;  freq['f'] = 5;
    const CodeLengths len = hf::buildCodeLengths(freq);
    EXPECT_EQ(len, lengthsOf({{'a', 1}, {'b', 3}, {'c', 3}, {'d', 3}, {'e', 4}, {'f', 4}}));
    EXPECT_EQ(cost(freq, len), 224u);
}

TEST(BuildCodeLengthsTest, AbracadabraCounts) {
    const CodeLengths len = hf::buildCodeLengths(hf::countFrequencies(ts::bytesOf("abracadabra")));
    EXPECT_EQ(len, lengthsOf({{'a', 1}, {'b', 3}, {'c', 3}, {'d', 3}, {'r', 3}}));
}

TEST(BuildCodeLengthsTest, EqualWeightsTieBreakInSymbolOrder) {
    // Stable sort keeps equal leaves in symbol order, so the two lowest symbols
    // merge first and sit deeper.
    FrequencyTable freq{};
    freq[10] = freq[20] = freq[30] = 1;
    EXPECT_EQ(hf::buildCodeLengths(freq), lengthsOf({{10, 2}, {20, 2}, {30, 1}}));
}

TEST(BuildCodeLengthsTest, LeafVersusMergedTiePrefersTheLeaf) {
    // {1,1,2,2}: both {2,2,2,2} and {3,3,2,1} cost 12. Taking the leaf on a tie
    // gives the flat tree (minimum variance, shortest longest code).
    FrequencyTable freq{};
    freq[0] = 1; freq[1] = 1; freq[2] = 2; freq[3] = 2;
    EXPECT_EQ(hf::buildCodeLengths(freq), lengthsOf({{0, 2}, {1, 2}, {2, 2}, {3, 2}}));
}

TEST(BuildCodeLengthsTest, SymbolPositionDoesNotChangeLengthsForDistinctWeights) {
    FrequencyTable low{}, high{};
    const uint64_t weights[] = {45, 13, 12, 16, 9, 5};
    for (int i = 0; i < 6; ++i) {
        low[i] = weights[i];
        high[250 - 40 * i] = weights[i];
    }
    const CodeLengths a = hf::buildCodeLengths(low);
    const CodeLengths b = hf::buildCodeLengths(high);
    for (int i = 0; i < 6; ++i) EXPECT_EQ(a[i], b[250 - 40 * i]) << "weight " << weights[i];
}

TEST(BuildCodeLengthsTest, FibonacciSixSymbols) {
    EXPECT_EQ(hf::buildCodeLengths(fibFreq(6)), lengthsOf({{0, 5}, {1, 5}, {2, 4}, {3, 3}, {4, 2}, {5, 1}}));
}

TEST(BuildCodeLengthsTest, FibonacciNinetySymbolsReachDepthEightyNine) {
    const int n = 90;
    const CodeLengths len = hf::buildCodeLengths(fibFreq(n));
    EXPECT_EQ(maxLen(len), n - 1);
    EXPECT_EQ(len[0], n - 1);
    for (int k = 1; k < n; ++k) EXPECT_EQ(len[k], n - k) << "symbol " << k;
    for (int k = n; k < 256; ++k) EXPECT_EQ(len[k], 0) << "symbol " << k;
    EXPECT_TRUE(kraftComplete(len));
}

TEST(BuildCodeLengthsTest, HugeWeightsDoNotOverflow) {
    FrequencyTable freq{};
    for (int s : {3, 60, 200, 255}) freq[s] = (1ull << 62) - 1;   // total just under 2^64
    EXPECT_EQ(hf::buildCodeLengths(freq), lengthsOf({{3, 2}, {60, 2}, {200, 2}, {255, 2}}));
}

TEST(BuildCodeLengthsTest, IsDeterministic) {
    std::mt19937_64 rng(5);
    for (Dist d : {Dist::Uniform, Dist::Sparse, Dist::Skewed, Dist::Extreme}) {
        for (int i = 0; i < 25; ++i) {
            const FrequencyTable freq = randomFrequencies(rng, d);
            EXPECT_EQ(hf::buildCodeLengths(freq), hf::buildCodeLengths(freq)) << distName(d);
        }
    }
}

TEST(BuildCodeLengthsTest, ScalingEveryWeightKeepsTheLengths) {
    std::mt19937_64 rng(6);
    for (int trial = 0; trial < 200; ++trial) {
        const FrequencyTable freq = randomFrequencies(rng, trial % 2 ? Dist::Uniform : Dist::Sparse);
        FrequencyTable scaled = freq;
        const uint64_t k = 2 + rng() % 1000;
        for (auto& f : scaled) f *= k;
        EXPECT_EQ(hf::buildCodeLengths(freq), hf::buildCodeLengths(scaled)) << "factor " << k;
    }
}

// Every symbol on its own gets a 1-bit code, whatever its weight.
class SingleSymbolTest : public ::testing::TestWithParam<int> {};

TEST_P(SingleSymbolTest, GetsLengthOne) {
    const int s = GetParam();
    for (uint64_t w : {1ull, 2ull, 1000ull, 1ull << 40, ~0ull}) {
        FrequencyTable freq{};
        freq[s] = w;
        EXPECT_EQ(hf::buildCodeLengths(freq), lengthsOf({{s, 1}})) << "weight " << w;
    }
}

TEST_P(SingleSymbolTest, CodeIsZeroAndDecodes) {
    const int s = GetParam();
    const Bytes data(37, static_cast<uint8_t>(s));
    const Encoded e = encode(data);
    EXPECT_EQ(e.codes[s], 0u);
    EXPECT_EQ(e.stream, Bytes(5, 0x00));
    expectRoundTrip(data);
}

INSTANTIATE_TEST_SUITE_P(EverySymbol, SingleSymbolTest, ::testing::Range(0, 256));

// n symbols of equal weight: a complete tree with lengths k and k+1 only,
// where k = floor(log2 n); the lowest symbols take the longer codes.
class EqualWeightsTest : public ::testing::TestWithParam<int> {};

TEST_P(EqualWeightsTest, LengthsAreFloorAndCeilOfLog2) {
    const int n = GetParam();
    int k = 0;
    while ((2 << k) <= n) ++k;   // 2^k <= n < 2^(k+1)
    const int longer = 2 * n - (2 << k);   // symbols at depth k+1
    const int shorter = n - longer;          // symbols at depth k

    for (uint64_t w : {1ull, 7ull, 1ull << 50}) {
        FrequencyTable freq{};
        for (int s = 0; s < n; ++s) freq[s] = w;
        const CodeLengths len = hf::buildCodeLengths(freq);

        int atK = 0, atK1 = 0;
        for (int s = 0; s < n; ++s) {
            if (len[s] == k) ++atK;
            else if (len[s] == k + 1) ++atK1;
            else ADD_FAILURE() << "symbol " << s << " has length " << int(len[s]) << ", want " << k << " or " << k + 1;
        }
        EXPECT_EQ(atK, shorter) << "weight " << w;
        EXPECT_EQ(atK1, longer) << "weight " << w;
        EXPECT_TRUE(kraftComplete(len));
        for (int s = 1; s < n; ++s) EXPECT_GE(len[s - 1], len[s]) << "tie-break is by symbol value";
        EXPECT_EQ(cost(freq, len), referenceHuffmanCost(freq));
    }
}

INSTANTIATE_TEST_SUITE_P(TwoTo256, EqualWeightsTest, ::testing::Range(2, 257));

// Seeded random tables: optimal, complete, monotone in weight.
class RandomTablesTest : public ::testing::TestWithParam<std::tuple<Dist, int>> {};

TEST_P(RandomTablesTest, AreOptimalCompleteAndMonotone) {
    const Dist dist = std::get<0>(GetParam());
    std::mt19937_64 rng(static_cast<uint64_t>(std::get<1>(GetParam())) * 1000003 + static_cast<int>(dist));

    for (int t = 0; t < 10; ++t) {
        const FrequencyTable freq = randomFrequencies(rng, dist);
        const CodeLengths len = hf::buildCodeLengths(freq);
        int active = 0;
        for (int s = 0; s < 256; ++s) {
            ASSERT_EQ(len[s] != 0, freq[s] != 0) << "symbol " << s;
            active += freq[s] != 0;
        }
        EXPECT_EQ(cost(freq, len), referenceHuffmanCost(freq)) << "not optimal";
        EXPECT_TRUE(kraftComplete(len)) << "a Huffman tree is always full";
        EXPECT_LE(maxLen(len), active - 1);
        for (int a = 0; a < 256; ++a) {
            if (!freq[a]) continue;
            for (int b = 0; b < 256; ++b) {
                if (freq[b] && freq[a] > freq[b]) ASSERT_LE(len[a], len[b]) << "heavier " << a << " got a longer code than " << b;
            }
        }
    }
}

INSTANTIATE_TEST_SUITE_P(DistsAndSeeds, RandomTablesTest,
                         ::testing::Combine(::testing::Values(Dist::Uniform, Dist::Sparse, Dist::Skewed, Dist::Extreme),
                                            ::testing::Range(0, 40)),
                         [](const auto& info) {
                             return std::string(distName(std::get<0>(info.param))) + "_" +
                                    std::to_string(std::get<1>(info.param));
                         });

// ===========================================================================
// LimitCodeLengths
// ===========================================================================
TEST(LimitCodeLengthsTest, AllZerosAreReturnedUnchanged) {
    EXPECT_EQ(hf::LimitCodeLengths(CodeLengths{}), CodeLengths{});
}

TEST(LimitCodeLengthsTest, ShortLengthsAreReturnedUnchanged) {
    std::mt19937_64 rng(7);
    for (int trial = 0; trial < 300; ++trial) {
        const CodeLengths len = hf::buildCodeLengths(randomFrequencies(rng, Dist::Uniform));
        if (maxLen(len) > hf::MAX_CODE_LEN) continue;
        EXPECT_EQ(hf::LimitCodeLengths(len), len);
    }
}

TEST(LimitCodeLengthsTest, ExactlyFifteenIsNotTouched) {
    const CodeLengths len = hf::buildCodeLengths(fibFreq(16));   // 15 deep: at the cap
    ASSERT_EQ(maxLen(len), 15);
    EXPECT_EQ(hf::LimitCodeLengths(len), len);
}

TEST(LimitCodeLengthsTest, FibonacciSeventeenExactResult) {
    // Before: len[k] = 17-k for k >= 1, len[0] = 16 (two codes at depth 16).
    // Folding 16 -> 15 overfills Kraft by one slot; one depth-14 leaf splits
    // into two depth-15 leaves. After: lengths 1..13 once, 15 four times.
    const CodeLengths before = hf::buildCodeLengths(fibFreq(17));
    ASSERT_EQ(before[0], 16);
    ASSERT_EQ(before[1], 16);
    ASSERT_EQ(before[2], 15);
    ASSERT_EQ(before[3], 14);

    CodeLengths want{};
    for (int k = 4; k <= 16; ++k) want[k] = static_cast<uint8_t>(17 - k);
    for (int k = 0; k <= 3; ++k) want[k] = 15;

    const CodeLengths after = hf::LimitCodeLengths(before);
    EXPECT_EQ(after, want);
    EXPECT_EQ(kraftSum15(after), 1u << hf::MAX_CODE_LEN);
}

TEST(LimitCodeLengthsTest, SingleOverlongEntryIsClampedToFifteen) {
    for (int l : {16, 17, 20, 100, 255}) {
        EXPECT_EQ(hf::LimitCodeLengths(lengthsOf({{77, l}})), lengthsOf({{77, 15}})) << l;
    }
}

TEST(LimitCodeLengthsTest, MaximumLengthByteIsHandled) {
    const CodeLengths after = hf::LimitCodeLengths(lengthsOf({{0, 1}, {1, 255}, {2, 255}}));
    EXPECT_EQ(after[0], 1);
    EXPECT_EQ(after[1], 15);
    EXPECT_EQ(after[2], 15);
    EXPECT_TRUE(hf::lengthsAreValid(after));
}

TEST(LimitCodeLengthsTest, AllSymbolsInADeepChainStillFit) {
    // 256 symbols, the deepest possible Huffman tree shape: 1, 2, ..., 254, 255, 255.
    CodeLengths len{};
    for (int s = 0; s < 255; ++s) len[s] = static_cast<uint8_t>(s + 1);
    len[255] = 255;
    ASSERT_TRUE(kraftComplete(len));
    const CodeLengths after = hf::LimitCodeLengths(len);
    EXPECT_TRUE(hf::lengthsAreValid(after));
    EXPECT_EQ(activeSymbols(after), 256);
    EXPECT_EQ(kraftSum15(after), 1u << 15);
    EXPECT_EQ(maxLen(after), 15);
}

// Fibonacci counts for 2..90 symbols: depth n-1 before the cap.
class FibonacciLimitTest : public ::testing::TestWithParam<int> {};

TEST_P(FibonacciLimitTest, IsCappedCompleteAndOrderPreserving) {
    const int n = GetParam();
    const FrequencyTable freq = fibFreq(n);
    const CodeLengths before = hf::buildCodeLengths(freq);
    const CodeLengths after = hf::LimitCodeLengths(before);

    ASSERT_EQ(maxLen(before), n - 1);
    EXPECT_TRUE(hf::lengthsAreValid(after));
    EXPECT_LE(maxLen(after), hf::MAX_CODE_LEN);
    EXPECT_EQ(activeSymbols(after), n);
    EXPECT_EQ(kraftSum15(after), 1u << hf::MAX_CODE_LEN) << "the limiter must keep the code complete";
    if (maxLen(before) <= hf::MAX_CODE_LEN) EXPECT_EQ(after, before);
    for (int a = 0; a < n; ++a)
        for (int b = 0; b < n; ++b)
            if (before[a] < before[b]) ASSERT_LE(after[a], after[b]) << a << " vs " << b;
    EXPECT_GE(cost(freq, after), cost(freq, before)) << "limiting can only cost bits";
}

INSTANTIATE_TEST_SUITE_P(TwoTo90Symbols, FibonacciLimitTest, ::testing::Range(2, 91));

// Random tree shapes, complete and with leaves dropped, deep and shallow.
class RandomTreeLimitTest : public ::testing::TestWithParam<int> {};

TEST_P(RandomTreeLimitTest, AnyValidTreeIsCappedWithoutLosingASymbol) {
    std::mt19937_64 rng(static_cast<uint64_t>(GetParam()) * 7727 + 1);
    for (int t = 0; t < 6; ++t) {
        const int n = 2 + static_cast<int>(rng() % 255);
        CodeLengths before = randomTreeLengths(rng, n, t % 2 == 0);
        const bool complete = t % 3 != 2;
        if (!complete) {   // drop a few leaves: Kraft < 1 but still valid
            for (int drop = 0; drop < 3; ++drop) {
                const int s = static_cast<int>(rng() % 256);
                if (activeSymbols(before) > 2) before[s] = 0;
            }
        }
        ASSERT_TRUE(kraftAtMostOne(before));

        const CodeLengths after = hf::LimitCodeLengths(before);
        EXPECT_TRUE(hf::lengthsAreValid(after));
        EXPECT_LE(maxLen(after), hf::MAX_CODE_LEN);
        for (int s = 0; s < 256; ++s) ASSERT_EQ(after[s] != 0, before[s] != 0) << "symbol " << s;
        if (maxLen(before) <= hf::MAX_CODE_LEN) EXPECT_EQ(after, before);
        if (kraftComplete(before)) EXPECT_EQ(kraftSum15(after), 1u << 15);
        for (int a = 0; a < 256; ++a) {
            if (!before[a]) continue;
            for (int b = 0; b < 256; ++b)
                if (before[b] && before[a] < before[b]) ASSERT_LE(after[a], after[b]) << a << " vs " << b;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Seeds, RandomTreeLimitTest, ::testing::Range(0, 150));

// ===========================================================================
// lengthsAreValid
// ===========================================================================
TEST(LengthsAreValidTest, AllZerosIsValid) {
    EXPECT_TRUE(hf::lengthsAreValid(CodeLengths{}));
}

TEST(LengthsAreValidTest, SmallHandPickedCodes) {
    EXPECT_TRUE(hf::lengthsAreValid(lengthsOf({{0, 1}, {1, 1}})));
    EXPECT_TRUE(hf::lengthsAreValid(lengthsOf({{0, 1}, {1, 2}, {2, 2}})));
    EXPECT_FALSE(hf::lengthsAreValid(lengthsOf({{0, 1}, {1, 1}, {2, 1}})));
    EXPECT_FALSE(hf::lengthsAreValid(lengthsOf({{0, 1}, {1, 1}, {2, 2}})));
}

TEST(LengthsAreValidTest, IncompleteCodeIsValid) {
    EXPECT_TRUE(hf::lengthsAreValid(lengthsOf({{9, 1}})));
    EXPECT_TRUE(hf::lengthsAreValid(lengthsOf({{0, 1}, {1, 3}})));
}

TEST(LengthsAreValidTest, AllSymbolsAtEightExactlyFillsTheSpace) {
    CodeLengths len;
    len.fill(8);
    EXPECT_TRUE(hf::lengthsAreValid(len));
}

TEST(LengthsAreValidTest, OneSevenPlus255EightsOverfills) {
    CodeLengths len;
    len.fill(8);
    len[0] = 7;
    EXPECT_FALSE(hf::lengthsAreValid(len));
}

TEST(LengthsAreValidTest, EverySymbolAtFifteenIsValid) {
    CodeLengths len;
    len.fill(15);
    EXPECT_TRUE(hf::lengthsAreValid(len));
}

TEST(LengthsAreValidTest, AgreesWithAnExactKraftCheckOnRandomTables) {
    std::mt19937_64 rng(8);
    int valid = 0, invalid = 0;
    for (int trial = 0; trial < 20000; ++trial) {
        CodeLengths len{};
        const int n = 1 + static_cast<int>(rng() % 20);
        for (int i = 0; i < n; ++i) len[rng() % 256] = static_cast<uint8_t>(rng() % 18);
        const bool want = maxLen(len) <= 15 && kraftAtMostOne(len);
        ASSERT_EQ(hf::lengthsAreValid(len), want) << "trial " << trial;
        (want ? valid : invalid)++;
    }
    EXPECT_GT(valid, 1000);
    EXPECT_GT(invalid, 1000);
}

// Any single length alone: valid iff it is at most 15.
class LengthValueTest : public ::testing::TestWithParam<int> {};

TEST_P(LengthValueTest, SingleLengthIsValidIffAtMostFifteen) {
    const int l = GetParam();
    for (int s : {0, (l * 37) % 256, 255}) {
        CodeLengths len{};
        len[s] = static_cast<uint8_t>(l);
        EXPECT_EQ(hf::lengthsAreValid(len), l <= 15) << "symbol " << s;
    }
}

INSTANTIATE_TEST_SUITE_P(EveryByte, LengthValueTest, ::testing::Range(0, 256));

// A chain 1, 2, ..., L-1, L, L sums to exactly 1: valid; one more code overfills.
class KraftChainTest : public ::testing::TestWithParam<int> {};

TEST_P(KraftChainTest, ExactlyFullIsValidOneMoreIsNot) {
    const int L = GetParam();
    CodeLengths len{};
    for (int l = 1; l < L; ++l) len[l] = static_cast<uint8_t>(l);
    len[100] = static_cast<uint8_t>(L);
    len[101] = static_cast<uint8_t>(L);
    EXPECT_TRUE(hf::lengthsAreValid(len));
    EXPECT_EQ(kraftSum15(len), 1u << 15);

    CodeLengths over = len;
    over[200] = 15;
    EXPECT_FALSE(hf::lengthsAreValid(over)) << "one 15-bit code past a full tree";

    CodeLengths under = len;
    under[101] = 0;
    EXPECT_TRUE(hf::lengthsAreValid(under));
}

INSTANTIATE_TEST_SUITE_P(OneToFifteen, KraftChainTest, ::testing::Range(1, 16));

// 2^L codes of length L fill the space, for every L that fits 256 symbols.
class FullLevelTest : public ::testing::TestWithParam<int> {};

TEST_P(FullLevelTest, FullLevelIsValidAndAnExtraCodeIsNot) {
    const int L = GetParam();
    CodeLengths len{};
    for (int s = 0; s < (1 << L); ++s) len[s] = static_cast<uint8_t>(L);
    EXPECT_TRUE(hf::lengthsAreValid(len));
    if ((1 << L) < 256) {
        len[255] = 15;
        EXPECT_FALSE(hf::lengthsAreValid(len));
    }
}

INSTANTIATE_TEST_SUITE_P(OneToEight, FullLevelTest, ::testing::Range(1, 9));

// ===========================================================================
// maxLength
// ===========================================================================
TEST(MaxLengthTest, AllZerosIsZero) {
    EXPECT_EQ(hf::maxLength(CodeLengths{}), 0);
}

TEST(MaxLengthTest, FindsTheLongestAnywhere) {
    for (int s = 0; s < 256; ++s) {
        CodeLengths len{};
        len[(s + 1) % 256] = 3;
        len[s] = 9;
        EXPECT_EQ(hf::maxLength(len), 9) << s;
    }
}

TEST(MaxLengthTest, Reads255) {
    EXPECT_EQ(hf::maxLength(lengthsOf({{4, 255}})), 255);
}

TEST(MaxLengthTest, MatchesStdMaxOnRandomTables) {
    std::mt19937_64 rng(9);
    for (int trial = 0; trial < 2000; ++trial) {
        CodeLengths len{};
        for (auto& l : len) l = static_cast<uint8_t>(rng() % 3 == 0 ? rng() % 256 : 0);
        ASSERT_EQ(hf::maxLength(len), maxLen(len));
    }
}

// ===========================================================================
// buildCanonicalCodes
// ===========================================================================
TEST(BuildCanonicalCodesTest, InvalidLengthsReturnFalseAndLeaveCodesUntouched) {
    const std::vector<CodeLengths> invalid = {
        lengthsOf({{0, 1}, {1, 1}, {2, 1}}),     // Kraft 3/2
        lengthsOf({{5, 16}}),                    // too long
        lengthsOf({{0, 255}}),
        lengthsOf({{0, 1}, {1, 2}, {2, 2}, {3, 15}}),
    };
    for (std::size_t i = 0; i < invalid.size(); ++i) {
        CodeTable codes;
        codes.fill(0xDEADBEEF);
        EXPECT_FALSE(hf::buildCanonicalCodes(invalid[i], codes)) << "table " << i;
        for (int s = 0; s < 256; ++s) ASSERT_EQ(codes[s], 0xDEADBEEFu) << "table " << i << " symbol " << s;
    }
}

TEST(BuildCanonicalCodesTest, AllZeroLengthsReturnFalseAndLeaveCodesUntouched) {
    // Huffman.hpp: an all-zero table is nothing to encode, so not a usable code,
    // even though it passes lengthsAreValid (Kraft sum 0).
    ASSERT_TRUE(hf::lengthsAreValid(CodeLengths{}));
    CodeTable codes;
    codes.fill(0xDEADBEEF);
    EXPECT_FALSE(hf::buildCanonicalCodes(CodeLengths{}, codes));
    for (int s = 0; s < 256; ++s) ASSERT_EQ(codes[s], 0xDEADBEEFu) << "symbol " << s;
}

TEST(BuildCanonicalCodesTest, SingleSymbolGetsCodeZero) {
    for (int s = 0; s < 256; ++s) {
        CodeTable codes;
        codes.fill(7);
        ASSERT_TRUE(hf::buildCanonicalCodes(lengthsOf({{s, 1}}), codes));
        for (int t = 0; t < 256; ++t) ASSERT_EQ(codes[t], 0u) << "symbol " << s << " entry " << t;
    }
}

TEST(BuildCanonicalCodesTest, Rfc1951Example) {
    // RFC 1951 3.2.2: ABCDEFGH with lengths (3,3,3,3,3,2,4,4).
    const CodeLengths len = lengthsOf(
        {{'A', 3}, {'B', 3}, {'C', 3}, {'D', 3}, {'E', 3}, {'F', 2}, {'G', 4}, {'H', 4}});
    CodeTable codes{};
    ASSERT_TRUE(hf::buildCanonicalCodes(len, codes));
    EXPECT_EQ(codes['A'], 0b010u);
    EXPECT_EQ(codes['B'], 0b011u);
    EXPECT_EQ(codes['C'], 0b100u);
    EXPECT_EQ(codes['D'], 0b101u);
    EXPECT_EQ(codes['E'], 0b110u);
    EXPECT_EQ(codes['F'], 0b00u);
    EXPECT_EQ(codes['G'], 0b1110u);
    EXPECT_EQ(codes['H'], 0b1111u);
}

TEST(BuildCanonicalCodesTest, ClrsLengths) {
    const CodeLengths len = lengthsOf({{'a', 1}, {'b', 3}, {'c', 3}, {'d', 3}, {'e', 4}, {'f', 4}});
    CodeTable codes{};
    ASSERT_TRUE(hf::buildCanonicalCodes(len, codes));
    EXPECT_EQ(codes['a'], 0b0u);
    EXPECT_EQ(codes['b'], 0b100u);
    EXPECT_EQ(codes['c'], 0b101u);
    EXPECT_EQ(codes['d'], 0b110u);
    EXPECT_EQ(codes['e'], 0b1110u);
    EXPECT_EQ(codes['f'], 0b1111u);
}

TEST(BuildCanonicalCodesTest, IncompleteCodeLeavesAGap) {
    CodeTable codes{};
    ASSERT_TRUE(hf::buildCanonicalCodes(lengthsOf({{0, 1}, {1, 3}}), codes));
    EXPECT_EQ(codes[0], 0b0u);
    EXPECT_EQ(codes[1], 0b100u);
}

TEST(BuildCanonicalCodesTest, SameLengthCodesFollowSymbolValue) {
    CodeTable codes{};
    ASSERT_TRUE(hf::buildCanonicalCodes(lengthsOf({{200, 1}, {5, 1}}), codes));
    EXPECT_EQ(codes[5], 0u);
    EXPECT_EQ(codes[200], 1u);
}

TEST(BuildCanonicalCodesTest, AllSymbolsAtEightMapToThemselves) {
    CodeLengths len;
    len.fill(8);
    CodeTable codes{};
    ASSERT_TRUE(hf::buildCanonicalCodes(len, codes));
    for (int s = 0; s < 256; ++s) EXPECT_EQ(codes[s], static_cast<uint32_t>(s));
}

TEST(BuildCanonicalCodesTest, EverySymbolAtFifteenCountsUpFromZero) {
    CodeLengths len;
    len.fill(15);
    CodeTable codes{};
    ASSERT_TRUE(hf::buildCanonicalCodes(len, codes));
    for (int s = 0; s < 256; ++s) EXPECT_EQ(codes[s], static_cast<uint32_t>(s));
}

TEST(BuildCanonicalCodesTest, FifteenBitCodesFromTheLimiter) {
    const CodeLengths len = hf::LimitCodeLengths(hf::buildCodeLengths(fibFreq(17)));
    CodeTable codes{};
    ASSERT_TRUE(hf::buildCanonicalCodes(len, codes));
    // symbols 0..3 share the last four 15-bit codes, in symbol order
    EXPECT_EQ(codes[0], 0x7FFCu);
    EXPECT_EQ(codes[1], 0x7FFDu);
    EXPECT_EQ(codes[2], 0x7FFEu);
    EXPECT_EQ(codes[3], 0x7FFFu);
    EXPECT_EQ(codes[16], 0u);   // the single length-1 code
    expectCanonical(len, codes);
}

TEST(BuildCanonicalCodesTest, OnlyDependsOnTheLengths) {
    std::mt19937_64 rng(10);
    for (int trial = 0; trial < 100; ++trial) {
        const CodeLengths len = hf::LimitCodeLengths(hf::buildCodeLengths(randomFrequencies(rng, Dist::Skewed)));
        CodeTable a{}, b;
        b.fill(0x12345678);
        ASSERT_TRUE(hf::buildCanonicalCodes(len, a));
        ASSERT_TRUE(hf::buildCanonicalCodes(len, b));
        EXPECT_EQ(a, b) << "old table contents must not leak into the result";
    }
}

// Every seed: lengths from random frequencies, then every canonical property.
class RandomCanonicalTest : public ::testing::TestWithParam<std::tuple<Dist, int>> {};

TEST_P(RandomCanonicalTest, CodesFollowTheCanonicalRule) {
    const Dist dist = std::get<0>(GetParam());
    std::mt19937_64 rng(static_cast<uint64_t>(std::get<1>(GetParam())) * 65537 + static_cast<int>(dist));
    for (int t = 0; t < 4; ++t) {
        const CodeLengths len = hf::LimitCodeLengths(hf::buildCodeLengths(randomFrequencies(rng, dist)));
        CodeTable codes{};
        ASSERT_TRUE(hf::buildCanonicalCodes(len, codes));
        expectCanonical(len, codes);
    }
}

TEST_P(RandomCanonicalTest, IncompleteRandomLengthsAreStillPrefixFree) {
    const Dist dist = std::get<0>(GetParam());
    std::mt19937_64 rng(static_cast<uint64_t>(std::get<1>(GetParam())) * 92821 + static_cast<int>(dist));
    for (int t = 0; t < 4; ++t) {
        CodeLengths len = hf::LimitCodeLengths(hf::buildCodeLengths(randomFrequencies(rng, dist)));
        for (int drop = 0; drop < 3 && activeSymbols(len) > 1; ++drop) {
            const int s = static_cast<int>(rng() % 256);
            len[s] = 0;
        }
        CodeTable codes{};
        ASSERT_TRUE(hf::buildCanonicalCodes(len, codes));
        expectCanonical(len, codes);
    }
}

INSTANTIATE_TEST_SUITE_P(DistsAndSeeds, RandomCanonicalTest,
                         ::testing::Combine(::testing::Values(Dist::Uniform, Dist::Sparse, Dist::Skewed, Dist::Extreme),
                                            ::testing::Range(0, 50)),
                         [](const auto& info) {
                             return std::string(distName(std::get<0>(info.param))) + "_" +
                                    std::to_string(std::get<1>(info.param));
                         });

// ===========================================================================
// Full pipeline: countFrequencies -> buildCodeLengths -> LimitCodeLengths ->
// buildCanonicalCodes -> BitWriter -> BitReader -> original bytes.
// ===========================================================================
TEST(HuffmanPipelineTest, AbracadabraGoldenBytes) {
    // a:5 b:2 r:2 c:1 d:1 -> a=0, b=100, c=101, d=110, r=111
    // a b   r   a c   a d   a b   r   a
    // 0 100 111 0 101 0 110 0 100 111 0  ->  01001110 10101100 1001110(0)
    const Bytes data = ts::bytesOf("abracadabra");
    const Encoded e = encode(data);
    EXPECT_EQ(e.len, lengthsOf({{'a', 1}, {'b', 3}, {'c', 3}, {'d', 3}, {'r', 3}}));
    EXPECT_EQ(e.codes['a'], 0b0u);
    EXPECT_EQ(e.codes['b'], 0b100u);
    EXPECT_EQ(e.codes['c'], 0b101u);
    EXPECT_EQ(e.codes['d'], 0b110u);
    EXPECT_EQ(e.codes['r'], 0b111u);
    EXPECT_EQ(e.bits_written, 23u);
    EXPECT_EQ(e.stream, (Bytes{0x4E, 0xAC, 0x9C}));
    expectRoundTrip(data);
}

TEST(HuffmanPipelineTest, EmptyInput) {
    const Encoded e = encode(Bytes{});
    EXPECT_FALSE(e.usable) << "no symbols, no code";
    EXPECT_TRUE(e.stream.empty());
    EXPECT_EQ(e.bits_written, 0u);
    EXPECT_EQ(e.len, CodeLengths{});
}

TEST(HuffmanPipelineTest, EveryByteValueUniformlyUsesEightBitCodes) {
    Bytes data;
    for (int rep = 0; rep < 4; ++rep)
        for (int s = 0; s < 256; ++s) data.push_back(static_cast<uint8_t>(s));
    const Encoded e = encode(data);
    for (int s = 0; s < 256; ++s) EXPECT_EQ(e.len[s], 8);
    EXPECT_EQ(e.stream, data) << "8-bit canonical codes over all 256 symbols are the identity";
    expectRoundTrip(data);
}

TEST(HuffmanPipelineTest, RareByteAtTheVeryEndUsesTheLongestCode) {
    // Fibonacci counts for symbols 0..19; symbol 0 occurs once (F(1)) and is 19
    // deep before the cap. Its only occurrence goes last, so the decoder meets
    // the longest code with the stream about to run out.
    Bytes data;
    const FrequencyTable f = fibFreq(20);
    for (int s = 1; s < 20; ++s) data.insert(data.end(), f[s], static_cast<uint8_t>(s));
    std::mt19937_64 rng(11);
    std::shuffle(data.begin(), data.end(), rng);
    data.push_back(0);
    const Encoded e = encode(data);
    ASSERT_EQ(e.unlimited[0], 19);
    EXPECT_EQ(e.len[0], hf::MAX_CODE_LEN);
    expectRoundTrip(data);
}

class PipelineShapeTest : public ::testing::TestWithParam<std::tuple<Shape, int>> {};

TEST_P(PipelineShapeTest, RoundTripsThroughBothDecoders) {
    const Shape shape = std::get<0>(GetParam());
    const int size = std::get<1>(GetParam());
    const Bytes data = ts::makeData(shape, size, 12);
    if (data.empty()) {
        const Encoded e = encode(data);
        EXPECT_TRUE(e.stream.empty());
        return;
    }
    expectRoundTrip(data);
}

TEST_P(PipelineShapeTest, CostNeverBeatsTheUnlimitedOptimum) {
    const Shape shape = std::get<0>(GetParam());
    const int size = std::get<1>(GetParam());
    const Bytes data = ts::makeData(shape, size, 13);
    const Encoded e = encode(data);
    EXPECT_EQ(cost(e.freq, e.unlimited), data.empty() ? 0 : referenceHuffmanCost(e.freq));
    EXPECT_GE(cost(e.freq, e.len), cost(e.freq, e.unlimited));
}

INSTANTIATE_TEST_SUITE_P(ShapesAndSizes, PipelineShapeTest,
                         ::testing::Combine(::testing::ValuesIn(ts::allShapes()),
                                            ::testing::Values(0, 1, 2, 3, 100, 1000, 10000, 100000)),
                         [](const auto& info) {
                             return std::string(ts::shapeName(std::get<0>(info.param))) + "_" +
                                    std::to_string(std::get<1>(info.param));
                         });
