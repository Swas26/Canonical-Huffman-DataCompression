// test_huffman.cpp -- hf:: from frequency counting to canonical codes, plus a
// full encode (BitWriter) -> decode (BitReader) round trip at the end.
//
// Hand-checked vectors (CLRS 16.3, RFC 1951 3.2.2, "abracadabra") pin exact
// output; seeded random tables check the properties any correct build must have.
#include "BitReader.hpp"
#include "BitWriter.hpp"
#include "Huffman.hpp"

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
using Bytes = std::vector<uint8_t>;

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Optimal prefix-code cost via a textbook min-heap Huffman: the cost of the tree
// is the sum of all merge weights. Independent of the two-queue implementation.
uint64_t referenceHuffmanCost(const FrequencyTable& freq) {
    std::priority_queue<uint64_t, std::vector<uint64_t>, std::greater<uint64_t>> pq;
    for (uint64_t f : freq) {
        if (f != 0) pq.push(f);
    }
    if (pq.size() == 1) return pq.top();   // lone symbol still costs 1 bit each
    uint64_t cost = 0;
    while (pq.size() > 1) {
        uint64_t a = pq.top(); pq.pop();
        uint64_t b = pq.top(); pq.pop();
        cost += a + b;
        pq.push(a + b);
    }
    return cost;
}

uint64_t cost(const FrequencyTable& freq, const CodeLengths& len) {
    uint64_t total = 0;
    for (int s = 0; s < 256; ++s) total += freq[s] * len[s];
    return total;
}

int maxLength(const CodeLengths& len) {
    return *std::max_element(len.begin(), len.end());
}

int activeSymbols(const CodeLengths& len) {
    return static_cast<int>(std::count_if(len.begin(), len.end(), [](uint8_t l) { return l != 0; }));
}

// Exact Kraft equality (sum 2^-l == 1) for any depth up to 255: fold the levels
// bottom-up, pairing siblings. Every level must pair off evenly and the fold
// must end at a single root.
bool kraftComplete(const CodeLengths& len) {
    std::array<uint64_t, 256> count{};
    for (uint8_t l : len) {
        if (l != 0) ++count[l];
    }
    uint64_t pending = 0;
    for (int level = 255; level >= 1; --level) {
        pending += count[level];
        if (pending % 2 != 0) return false;
        pending /= 2;
    }
    return pending == 1;
}

// Kraft sum in units of 2^-15. Only meaningful when every length is <= 15.
uint32_t kraftSum15(const CodeLengths& len) {
    uint32_t sum = 0;
    for (uint8_t l : len) {
        if (l != 0) sum += 1u << (hf::MAX_CODE_LEN - l);
    }
    return sum;
}

bool isPrefixFree(const CodeLengths& len, const CodeTable& codes) {
    for (int a = 0; a < 256; ++a) {
        if (len[a] == 0) continue;
        for (int b = 0; b < 256; ++b) {
            if (b == a || len[b] == 0 || len[a] > len[b]) continue;
            // a is a prefix of b when b's top len[a] bits equal a's code.
            if ((codes[b] >> (len[b] - len[a])) == codes[a]) return false;
        }
    }
    return true;
}

CodeLengths lengthsOf(std::initializer_list<std::pair<int, int>> symbolLength) {
    CodeLengths len{};
    for (const auto& sl : symbolLength) len[sl.first] = static_cast<uint8_t>(sl.second);
    return len;
}

// Symbol k gets weight F(k+1): 1, 1, 2, 3, 5, 8, ... The worst case for depth:
// n symbols make a tree n-1 deep. n <= 90 keeps the total inside uint64_t.
FrequencyTable fibFreq(int n) {
    FrequencyTable freq{};
    uint64_t a = 1, b = 1;
    for (int k = 0; k < n; ++k) {
        freq[k] = a;
        uint64_t next = a + b;
        a = b;
        b = next;
    }
    return freq;
}

enum class Dist { Uniform, Sparse, Skewed };

const char* distName(Dist d) {
    switch (d) {
        case Dist::Uniform: return "uniform";
        case Dist::Sparse:  return "sparse";
        case Dist::Skewed:  return "skewed";
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
        case Dist::Skewed:    // counts spread over 40 powers of two -> deep trees
            k = 16 + static_cast<int>(rng() % 241);
            for (int i = 0; i < k; ++i) freq[symbols[i]] = (1ull << (rng() % 40)) + rng() % 16;
            break;
    }
    return freq;
}

// ---------------------------------------------------------------------------
// Encode / decode used by the pipeline tests
// ---------------------------------------------------------------------------
struct Encoded {
    FrequencyTable freq{};
    CodeLengths unlimited{};
    CodeLengths len{};
    CodeTable codes{};
    Bytes stream;
    uint64_t bits_written = 0;
};

Encoded encode(const Bytes& data) {
    Encoded e;
    e.freq = hf::countFrequencies(data);
    e.unlimited = hf::buildCodeLengths(e.freq);
    e.len = hf::LimitCodeLengths(e.unlimited);
    e.codes = hf::buildCanonicalCodes(e.len);
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

// Decoder A: one bit at a time, looking the growing code up in a map built from
// the code table. Knows nothing about how canonical codes are assigned.
Decoded decodeBitByBit(const Encoded& e, std::size_t symbols) {
    std::map<std::pair<int, uint32_t>, uint8_t> lookup;
    for (int s = 0; s < 256; ++s) {
        if (e.len[s] != 0) lookup[{e.len[s], e.codes[s]}] = static_cast<uint8_t>(s);
    }

    Decoded d;
    BitReader r(e.stream);
    for (std::size_t i = 0; i < symbols && !d.bad_code; ++i) {
        uint32_t code = 0;
        bool found = false;
        for (int l = 1; l <= hf::MAX_CODE_LEN && !found; ++l) {
            code = (code << 1) | (r.read_bit() ? 1u : 0u);
            auto it = lookup.find({l, code});
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

// Decoder B: a 2^15-entry table indexed by a fixed 15-bit peek, then skip the
// real code length. Near the end of the stream the window overhangs the data,
// which must NOT count as an overrun.
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

    EXPECT_TRUE(hf::lengthsAreValid(e.len));
    EXPECT_LE(maxLength(e.len), hf::MAX_CODE_LEN);
    for (int s = 0; s < 256; ++s) {
        EXPECT_EQ(e.len[s] != 0, e.freq[s] != 0) << "symbol " << s;
    }
    EXPECT_TRUE(isPrefixFree(e.len, e.codes));
    EXPECT_EQ(e.bits_written, cost(e.freq, e.len));
    EXPECT_EQ(e.stream.size(), (e.bits_written + 7) / 8);

    const Decoded a = decodeBitByBit(e, data.size());
    EXPECT_FALSE(a.bad_code) << "bit-by-bit decoder hit an unknown code";
    EXPECT_TRUE(a.out == data) << "bit-by-bit decoder output differs from input";
    EXPECT_EQ(a.bits_read, e.bits_written);
    EXPECT_TRUE(a.ok);

    const Decoded b = decodeWithTable(e, data.size());
    EXPECT_FALSE(b.bad_code) << "table decoder hit an empty or overlapping slot";
    EXPECT_TRUE(b.out == data) << "table decoder output differs from input";
    EXPECT_EQ(b.bits_read, e.bits_written);
    EXPECT_TRUE(b.ok) << "a 15-bit peek overhanging the end must not flag overrun";
}

Bytes bytesOf(const std::string& s) {
    return Bytes(s.begin(), s.end());
}

}  // namespace

// ===========================================================================
// countFrequencies
// ===========================================================================
TEST(CountFrequenciesTest, EmptyVectorGivesAllZeros) {
    EXPECT_EQ(hf::countFrequencies(Bytes{}), FrequencyTable{});
}

TEST(CountFrequenciesTest, NullPointerGivesAllZerosEvenWithNonZeroSize) {
    EXPECT_EQ(hf::countFrequencies(nullptr, 5), FrequencyTable{});
}

TEST(CountFrequenciesTest, ZeroSizeWithValidPointerGivesAllZeros) {
    const uint8_t data[] = {1, 2, 3};
    EXPECT_EQ(hf::countFrequencies(data, 0), FrequencyTable{});
}

TEST(CountFrequenciesTest, CountsBAB) {
    FrequencyTable want{};
    want['B'] = 2;
    want['A'] = 1;
    EXPECT_EQ(hf::countFrequencies(bytesOf("BAB")), want);
}

TEST(CountFrequenciesTest, OnlyTheFirstSizeBytesAreCounted) {
    const uint8_t data[] = {7, 7, 9, 9, 9};
    FrequencyTable want{};
    want[7] = 2;
    want[9] = 1;
    EXPECT_EQ(hf::countFrequencies(data, 3), want);
}

TEST(CountFrequenciesTest, EveryByteValueOnceGivesAllOnes) {
    Bytes data(256);
    std::iota(data.begin(), data.end(), 0);
    FrequencyTable want;
    want.fill(1);
    EXPECT_EQ(hf::countFrequencies(data), want);
}

TEST(CountFrequenciesTest, ExtremeByteValuesAreCounted) {
    const Bytes data{0x00, 0xFF, 0x00, 0xFF, 0xFF};
    const FrequencyTable f = hf::countFrequencies(data);
    EXPECT_EQ(f[0x00], 2u);
    EXPECT_EQ(f[0xFF], 3u);
}

TEST(CountFrequenciesTest, LargeRunOfOneByte) {
    const Bytes data(1000000, 0xFF);
    FrequencyTable want{};
    want[0xFF] = 1000000;
    EXPECT_EQ(hf::countFrequencies(data), want);
}

TEST(CountFrequenciesTest, RandomDataMatchesStdCountAndSumsToSize) {
    std::mt19937 rng(12345);
    Bytes data(10007);
    for (auto& b : data) b = static_cast<uint8_t>(rng());

    const FrequencyTable f = hf::countFrequencies(data);
    uint64_t sum = 0;
    for (int v = 0; v < 256; ++v) {
        EXPECT_EQ(f[v], static_cast<uint64_t>(std::count(data.begin(), data.end(), v))) << "value " << v;
        sum += f[v];
    }
    EXPECT_EQ(sum, data.size());
}

TEST(CountFrequenciesTest, VectorOverloadMatchesPointerOverload) {
    const Bytes data = bytesOf("the quick brown fox jumps over the lazy dog");
    EXPECT_EQ(hf::countFrequencies(data), hf::countFrequencies(data.data(), data.size()));
}

// ===========================================================================
// buildCodeLengths
// ===========================================================================
TEST(BuildCodeLengthsTest, EmptyTableGivesAllZeros) {
    EXPECT_EQ(hf::buildCodeLengths(FrequencyTable{}), CodeLengths{});
}

TEST(BuildCodeLengthsTest, SingleSymbolGetsLengthOne) {
    FrequencyTable freq{};
    freq['a'] = 7;
    EXPECT_EQ(hf::buildCodeLengths(freq), lengthsOf({{'a', 1}}));
}

TEST(BuildCodeLengthsTest, SingleSymbolWithHugeCountGetsLengthOne) {
    FrequencyTable freq{};
    freq[0xFF] = UINT64_MAX;
    EXPECT_EQ(hf::buildCodeLengths(freq), lengthsOf({{0xFF, 1}}));
}

TEST(BuildCodeLengthsTest, TwoSymbolsGetLengthOneEach) {
    FrequencyTable freq{};
    freq[0] = 1;
    freq[255] = 1000000;
    EXPECT_EQ(hf::buildCodeLengths(freq), lengthsOf({{0, 1}, {255, 1}}));
}

TEST(BuildCodeLengthsTest, FourEqualWeightsGiveLengthTwo) {
    FrequencyTable freq{};
    for (int s : {3, 50, 100, 200}) freq[s] = 10;
    EXPECT_EQ(hf::buildCodeLengths(freq), lengthsOf({{3, 2}, {50, 2}, {100, 2}, {200, 2}}));
}

TEST(BuildCodeLengthsTest, AllSymbolsEqualWeightGiveLengthEight) {
    FrequencyTable freq;
    freq.fill(42);
    CodeLengths want;
    want.fill(8);
    EXPECT_EQ(hf::buildCodeLengths(freq), want);
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
    EXPECT_EQ(hf::buildCodeLengths(fibFreq(6)),
              lengthsOf({{0, 5}, {1, 5}, {2, 4}, {3, 3}, {4, 2}, {5, 1}}));
}

TEST(BuildCodeLengthsTest, FibonacciNinetySymbolsReachDepthEightyNine) {
    const int n = 90;
    const CodeLengths len = hf::buildCodeLengths(fibFreq(n));

    EXPECT_EQ(maxLength(len), n - 1);
    EXPECT_EQ(len[0], n - 1);
    for (int k = 1; k < n; ++k) EXPECT_EQ(len[k], n - k) << "symbol " << k;
    for (int k = n; k < 256; ++k) EXPECT_EQ(len[k], 0) << "symbol " << k;
    EXPECT_TRUE(kraftComplete(len));
}

TEST(BuildCodeLengthsTest, IsDeterministic) {
    std::mt19937_64 rng(5);
    for (Dist d : {Dist::Uniform, Dist::Sparse, Dist::Skewed}) {
        const FrequencyTable freq = randomFrequencies(rng, d);
        EXPECT_EQ(hf::buildCodeLengths(freq), hf::buildCodeLengths(freq)) << distName(d);
    }
}

TEST(BuildCodeLengthsTest, RandomTablesAreOptimalCompleteAndMonotone) {
    std::mt19937_64 rng(20260917);
    const Dist dists[] = {Dist::Uniform, Dist::Sparse, Dist::Skewed};

    for (int iter = 0; iter < 500 && !HasFailure(); ++iter) {
        const Dist dist = dists[iter % 3];
        SCOPED_TRACE(std::string("iteration ") + std::to_string(iter) + ", " + distName(dist));

        const FrequencyTable freq = randomFrequencies(rng, dist);
        const CodeLengths len = hf::buildCodeLengths(freq);

        for (int s = 0; s < 256; ++s) {
            ASSERT_EQ(len[s] != 0, freq[s] != 0) << "symbol " << s;
        }
        EXPECT_TRUE(kraftComplete(len)) << "a Huffman tree is a full binary tree";
        EXPECT_EQ(cost(freq, len), referenceHuffmanCost(freq)) << "not an optimal code";

        // Optimal codes never give a more frequent symbol a longer code.
        for (int a = 0; a < 256; ++a) {
            for (int b = 0; b < 256; ++b) {
                if (freq[a] > freq[b] && freq[b] != 0) {
                    ASSERT_LE(len[a], len[b]) << "freq[" << a << "]=" << freq[a]
                                              << " > freq[" << b << "]=" << freq[b];
                }
            }
        }
    }
}

// ===========================================================================
// LimitCodeLengths
// ===========================================================================
TEST(LimitCodeLengthsTest, AllZerosAreReturnedUnchanged) {
    EXPECT_EQ(hf::LimitCodeLengths(CodeLengths{}), CodeLengths{});
}

TEST(LimitCodeLengthsTest, ShortLengthsAreReturnedUnchanged) {
    const CodeLengths clrs = lengthsOf({{'a', 1}, {'b', 3}, {'c', 3}, {'d', 3}, {'e', 4}, {'f', 4}});
    EXPECT_EQ(hf::LimitCodeLengths(clrs), clrs);

    CodeLengths flat;
    flat.fill(8);
    EXPECT_EQ(hf::LimitCodeLengths(flat), flat);

    const CodeLengths single = lengthsOf({{9, 1}});
    EXPECT_EQ(hf::LimitCodeLengths(single), single);
}

TEST(LimitCodeLengthsTest, ExactlyFifteenIsNotTouched) {
    // fibFreq(16) is 15 deep: already at the cap.
    const CodeLengths len = hf::buildCodeLengths(fibFreq(16));
    ASSERT_EQ(maxLength(len), 15);
    EXPECT_EQ(hf::LimitCodeLengths(len), len);
}

TEST(LimitCodeLengthsTest, FibonacciSeventeenExactResult) {
    // Before: len[k] = 17-k for k >= 1, len[0] = 16  (two codes at depth 16).
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

TEST(LimitCodeLengthsTest, FibonacciNinetySymbolsAreCappedAndStayComplete) {
    const CodeLengths before = hf::buildCodeLengths(fibFreq(90));
    const CodeLengths after = hf::LimitCodeLengths(before);

    EXPECT_EQ(maxLength(after), hf::MAX_CODE_LEN);
    EXPECT_EQ(activeSymbols(after), 90);
    EXPECT_TRUE(hf::lengthsAreValid(after));
    EXPECT_TRUE(kraftComplete(after));
    EXPECT_EQ(kraftSum15(after), 1u << hf::MAX_CODE_LEN);
}

TEST(LimitCodeLengthsTest, SingleOverlongEntryIsClampedToFifteen) {
    EXPECT_EQ(hf::LimitCodeLengths(lengthsOf({{77, 20}})), lengthsOf({{77, 15}}));
}

TEST(LimitCodeLengthsTest, MaximumLengthByteIsHandled) {
    const CodeLengths after = hf::LimitCodeLengths(lengthsOf({{0, 1}, {1, 255}, {2, 255}}));
    EXPECT_EQ(after[0], 1);
    EXPECT_EQ(after[1], 15);
    EXPECT_EQ(after[2], 15);
    EXPECT_TRUE(hf::lengthsAreValid(after));
}

TEST(LimitCodeLengthsTest, RandomDeepTablesAreCappedValidCompleteAndOrderPreserving) {
    std::mt19937_64 rng(777);
    int deep_tables = 0;

    for (int iter = 0; iter < 400 && !HasFailure(); ++iter) {
        SCOPED_TRACE("iteration " + std::to_string(iter));

        const FrequencyTable freq = randomFrequencies(rng, Dist::Skewed);
        const CodeLengths before = hf::buildCodeLengths(freq);
        const CodeLengths after = hf::LimitCodeLengths(before);
        if (maxLength(before) > hf::MAX_CODE_LEN) ++deep_tables;

        EXPECT_LE(maxLength(after), hf::MAX_CODE_LEN);
        EXPECT_TRUE(hf::lengthsAreValid(after));
        EXPECT_TRUE(kraftComplete(after)) << "limiting must not leave unused code space";
        EXPECT_GE(cost(freq, after), cost(freq, before)) << "beat an optimal code?";
        EXPECT_EQ(hf::LimitCodeLengths(after), after) << "not idempotent";

        for (int s = 0; s < 256; ++s) {
            ASSERT_EQ(after[s] != 0, before[s] != 0) << "symbol " << s << " gained or lost a code";
        }
        for (int a = 0; a < 256; ++a) {
            for (int b = 0; b < 256; ++b) {
                if (before[a] != 0 && before[b] != 0 && before[a] < before[b]) {
                    ASSERT_LE(after[a], after[b]) << "order flipped for symbols " << a << ", " << b;
                }
            }
        }
    }
    EXPECT_GT(deep_tables, 0) << "generator never produced a tree deeper than 15";
}

// ===========================================================================
// lengthsAreValid
// ===========================================================================
TEST(LengthsAreValidTest, AllZerosIsValid) {
    EXPECT_TRUE(hf::lengthsAreValid(CodeLengths{}));
}

TEST(LengthsAreValidTest, SmallHandPickedCodes) {
    EXPECT_TRUE(hf::lengthsAreValid(lengthsOf({{0, 1}})));
    EXPECT_TRUE(hf::lengthsAreValid(lengthsOf({{0, 1}, {1, 1}})));
    EXPECT_FALSE(hf::lengthsAreValid(lengthsOf({{0, 1}, {1, 1}, {2, 1}})));
    EXPECT_TRUE(hf::lengthsAreValid(lengthsOf({{0, 1}, {1, 2}, {2, 2}})));
    EXPECT_FALSE(hf::lengthsAreValid(lengthsOf({{0, 1}, {1, 2}, {2, 2}, {3, 2}})));
}

TEST(LengthsAreValidTest, IncompleteCodeIsValid) {
    EXPECT_TRUE(hf::lengthsAreValid(lengthsOf({{0, 2}})));
    EXPECT_TRUE(hf::lengthsAreValid(lengthsOf({{0, 1}, {1, 3}})));
}

TEST(LengthsAreValidTest, AllSymbolsAtEightExactlyFillsTheSpace) {
    CodeLengths len;
    len.fill(8);
    EXPECT_TRUE(hf::lengthsAreValid(len));
}

TEST(LengthsAreValidTest, OneTooManyAtEightIsInvalid) {
    CodeLengths len;
    len.fill(8);
    len[0] = 7;   // claims two slots' worth, 257 total
    EXPECT_FALSE(hf::lengthsAreValid(len));
}

TEST(LengthsAreValidTest, LengthFifteenIsAllowed) {
    EXPECT_TRUE(hf::lengthsAreValid(lengthsOf({{0, 15}})));
}

TEST(LengthsAreValidTest, LengthsAboveFifteenAreRejected) {
    EXPECT_FALSE(hf::lengthsAreValid(lengthsOf({{0, 16}})));
    EXPECT_FALSE(hf::lengthsAreValid(lengthsOf({{0, 1}, {1, 32}})));
    EXPECT_FALSE(hf::lengthsAreValid(lengthsOf({{0, 255}})));   // no UB shift
}

TEST(LengthsAreValidTest, KraftBoundaryAtDepthFifteen) {
    // Lengths 1..14 once each leave exactly two depth-15 slots.
    CodeLengths full{};
    for (int l = 1; l <= 14; ++l) full[l] = static_cast<uint8_t>(l);
    full[15] = 15;
    full[16] = 15;
    EXPECT_TRUE(hf::lengthsAreValid(full));

    CodeLengths over = full;
    over[17] = 15;
    EXPECT_FALSE(hf::lengthsAreValid(over));
}

// ===========================================================================
// buildCanonicalCodes
// ===========================================================================
TEST(BuildCanonicalCodesTest, InvalidLengthsGiveAnAllZeroTable) {
    EXPECT_EQ(hf::buildCanonicalCodes(lengthsOf({{0, 1}, {1, 1}, {2, 1}})), CodeTable{});
    EXPECT_EQ(hf::buildCanonicalCodes(lengthsOf({{0, 16}})), CodeTable{});
}

TEST(BuildCanonicalCodesTest, AllZeroLengthsGiveAllZeroCodes) {
    EXPECT_EQ(hf::buildCanonicalCodes(CodeLengths{}), CodeTable{});
}

TEST(BuildCanonicalCodesTest, SingleSymbolGetsCodeZero) {
    const CodeTable codes = hf::buildCanonicalCodes(lengthsOf({{'x', 1}}));
    EXPECT_EQ(codes['x'], 0u);
}

TEST(BuildCanonicalCodesTest, Rfc1951Example) {
    // RFC 1951 3.2.2: ABCDEFGH with lengths (3,3,3,3,3,2,4,4).
    const CodeLengths len = lengthsOf(
        {{'A', 3}, {'B', 3}, {'C', 3}, {'D', 3}, {'E', 3}, {'F', 2}, {'G', 4}, {'H', 4}});
    const CodeTable codes = hf::buildCanonicalCodes(len);

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
    const CodeTable codes = hf::buildCanonicalCodes(len);

    EXPECT_EQ(codes['a'], 0b0u);
    EXPECT_EQ(codes['b'], 0b100u);
    EXPECT_EQ(codes['c'], 0b101u);
    EXPECT_EQ(codes['d'], 0b110u);
    EXPECT_EQ(codes['e'], 0b1110u);
    EXPECT_EQ(codes['f'], 0b1111u);
}

TEST(BuildCanonicalCodesTest, IncompleteCodeLeavesAGap) {
    const CodeTable codes = hf::buildCanonicalCodes(lengthsOf({{0, 1}, {1, 3}}));
    EXPECT_EQ(codes[0], 0b0u);
    EXPECT_EQ(codes[1], 0b100u);
}

TEST(BuildCanonicalCodesTest, SameLengthCodesFollowSymbolValueNotInsertionOrder) {
    const CodeTable codes = hf::buildCanonicalCodes(lengthsOf({{200, 1}, {5, 1}}));
    EXPECT_EQ(codes[5], 0u);
    EXPECT_EQ(codes[200], 1u);
}

TEST(BuildCanonicalCodesTest, AllSymbolsAtEightMapToThemselves) {
    CodeLengths len;
    len.fill(8);
    const CodeTable codes = hf::buildCanonicalCodes(len);
    for (int s = 0; s < 256; ++s) EXPECT_EQ(codes[s], static_cast<uint32_t>(s));
}

TEST(BuildCanonicalCodesTest, FifteenBitCodesFromTheLimiter) {
    const CodeLengths len = hf::LimitCodeLengths(hf::buildCodeLengths(fibFreq(17)));
    const CodeTable codes = hf::buildCanonicalCodes(len);
    // Symbols 0..3 share the last four 15-bit codes, in symbol order.
    EXPECT_EQ(codes[0], 0x7FFCu);
    EXPECT_EQ(codes[1], 0x7FFDu);
    EXPECT_EQ(codes[2], 0x7FFEu);
    EXPECT_EQ(codes[3], 0x7FFFu);
    EXPECT_EQ(codes[16], 0u);   // the single length-1 code
    EXPECT_TRUE(isPrefixFree(len, codes));
}

TEST(BuildCanonicalCodesTest, RandomValidLengthsGiveWellFormedCanonicalCodes) {
    std::mt19937_64 rng(4242);
    const Dist dists[] = {Dist::Uniform, Dist::Sparse, Dist::Skewed};

    for (int iter = 0; iter < 300 && !HasFailure(); ++iter) {
        const Dist dist = dists[iter % 3];
        SCOPED_TRACE(std::string("iteration ") + std::to_string(iter) + ", " + distName(dist));

        const CodeLengths len = hf::LimitCodeLengths(hf::buildCodeLengths(randomFrequencies(rng, dist)));
        ASSERT_TRUE(hf::lengthsAreValid(len));
        const CodeTable codes = hf::buildCanonicalCodes(len);

        // Every code fits its length; unused symbols stay zero.
        for (int s = 0; s < 256; ++s) {
            if (len[s] == 0) {
                EXPECT_EQ(codes[s], 0u) << "symbol " << s;
            } else {
                EXPECT_LT(codes[s], 1u << len[s]) << "symbol " << s;
            }
        }

        EXPECT_TRUE(isPrefixFree(len, codes));

        // Sort by (length, symbol): canonical order.
        std::vector<int> order;
        for (int s = 0; s < 256; ++s) {
            if (len[s] != 0) order.push_back(s);
        }
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return len[a] < len[b]; });

        EXPECT_EQ(codes[order.front()], 0u) << "first canonical code must be all zeros";

        for (std::size_t i = 1; i < order.size(); ++i) {
            const int prev = order[i - 1];
            const int cur = order[i];
            if (len[prev] == len[cur]) {
                EXPECT_EQ(codes[cur], codes[prev] + 1) << "same-length codes must be consecutive";
            }
            // Left-aligned to 15 bits, canonical codes strictly increase.
            const uint32_t prevAligned = codes[prev] << (hf::MAX_CODE_LEN - len[prev]);
            const uint32_t curAligned = codes[cur] << (hf::MAX_CODE_LEN - len[cur]);
            EXPECT_LT(prevAligned, curAligned) << "symbols " << prev << " then " << cur;
        }

        // A complete code ends on the all-ones code of the longest length.
        const int last = order.back();
        EXPECT_EQ(codes[last], (1u << len[last]) - 1);
    }
}

// ===========================================================================
// Full pipeline: countFrequencies -> buildCodeLengths -> LimitCodeLengths ->
// buildCanonicalCodes -> BitWriter -> BitReader -> original bytes.
// ===========================================================================
TEST(HuffmanPipelineTest, AbracadabraGoldenBytes) {
    // a:5 b:2 r:2 c:1 d:1 -> a=0, b=100, c=101, d=110, r=111
    // a b   r   a c   a d   a b   r   a
    // 0 100 111 0 101 0 110 0 100 111 0  ->  01001110 10101100 1001110(0)
    const Bytes data = bytesOf("abracadabra");
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
    EXPECT_TRUE(e.stream.empty());
    EXPECT_EQ(e.bits_written, 0u);
    expectRoundTrip(Bytes{});
}

TEST(HuffmanPipelineTest, SingleRepeatedByte) {
    const Bytes data(4, 'a');
    const Encoded e = encode(data);
    EXPECT_EQ(e.len['a'], 1);
    EXPECT_EQ(e.codes['a'], 0u);
    EXPECT_EQ(e.bits_written, 4u);
    EXPECT_EQ(e.stream, Bytes{0x00});
    expectRoundTrip(data);
}

TEST(HuffmanPipelineTest, SingleByteInput) {
    expectRoundTrip(Bytes{0x7F});
}

TEST(HuffmanPipelineTest, TwoDistinctBytes) {
    expectRoundTrip(Bytes{0x00, 0xFF, 0xFF, 0x00, 0xFF});
}

TEST(HuffmanPipelineTest, EnglishText) {
    expectRoundTrip(bytesOf(
        "It was the best of times, it was the worst of times, it was the age of wisdom, "
        "it was the age of foolishness, it was the epoch of belief, it was the epoch of "
        "incredulity, it was the season of Light, it was the season of Darkness."));
}

TEST(HuffmanPipelineTest, EveryByteValueUniformlyUsesEightBitCodes) {
    Bytes data;
    for (int rep = 0; rep < 4; ++rep) {
        for (int v = 0; v < 256; ++v) data.push_back(static_cast<uint8_t>(v));
    }
    std::shuffle(data.begin(), data.end(), std::mt19937(3));

    const Encoded e = encode(data);
    for (int s = 0; s < 256; ++s) ASSERT_EQ(e.len[s], 8) << "symbol " << s;
    EXPECT_EQ(e.stream, data) << "8-bit canonical codes are the identity";
    expectRoundTrip(data);
}

TEST(HuffmanPipelineTest, RandomSkewedBytes) {
    std::mt19937 rng(8);
    std::geometric_distribution<int> geo(0.05);
    Bytes data(100000);
    for (auto& b : data) b = static_cast<uint8_t>(std::min(geo(rng), 255));
    expectRoundTrip(data);
}

TEST(HuffmanPipelineTest, RandomUniformBytes) {
    std::mt19937 rng(9);
    Bytes data(50000);
    for (auto& b : data) b = static_cast<uint8_t>(rng());
    expectRoundTrip(data);
}

TEST(HuffmanPipelineTest, FibonacciCountsForceLengthLimiting) {
    // 24 symbols, symbol k occurring F(k+1) times: an unlimited tree 23 deep.
    const FrequencyTable counts = fibFreq(24);
    Bytes data;
    for (int k = 0; k < 24; ++k) {
        data.insert(data.end(), counts[k], static_cast<uint8_t>(k * 10 + 3));
    }
    std::shuffle(data.begin(), data.end(), std::mt19937(24));

    const Encoded e = encode(data);
    ASSERT_EQ(maxLength(e.unlimited), 23) << "test data no longer exercises the limiter";
    EXPECT_EQ(maxLength(e.len), hf::MAX_CODE_LEN);
    expectRoundTrip(data);
}

TEST(HuffmanPipelineTest, RareByteAtTheVeryEndUsesTheLongestCode) {
    // The longest code is last, so the 15-bit peek window sits exactly on the end.
    const FrequencyTable counts = fibFreq(20);
    Bytes data;
    for (int k = 19; k >= 0; --k) {
        data.insert(data.end(), counts[k], static_cast<uint8_t>(k));
    }
    const Encoded e = encode(data);
    ASSERT_EQ(e.len[data.back()], hf::MAX_CODE_LEN);
    expectRoundTrip(data);
}
