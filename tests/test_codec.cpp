// test_codec.cpp -- cd::encode, cd::decode (both decoders) and cd::messge.
//
// Encode is pinned with exact archives and checked against the rule it
// promises: Huffman when the payload comes out smaller, raw otherwise, never
// bigger than header + input. Decode is driven with hand-built archives into
// every Status, with codes of every length 1..15 on both sides of the 9-bit
// fast table, and with every shape of input at many sizes. The robustness
// suites cut every archive at every length and flip every bit of it: decode
// must never report Ok with the wrong bytes, and the table and bit-by-bit
// decoders must always return the same status and output.
#include "BitWriter.hpp"
#include "Codec.hpp"
#include "Format.hpp"
#include "Huffman.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <ostream>
#include <random>
#include <set>
#include <string>
#include <vector>

using cd::Decoder;
using cd::Status;
using ts::Bytes;
using ts::Shape;

namespace cd {
// Lets gtest print a Status by name instead of as raw bytes.
void PrintTo(Status s, std::ostream* os) {
    switch (s) {
        case Status::Ok:          *os << "Ok"; return;
        case Status::BadHeader:   *os << "BadHeader"; return;
        case Status::BadLength:   *os << "BadLength"; return;
        case Status::BadCode:     *os << "BadCode"; return;
        case Status::Truncated:   *os << "Truncated"; return;
        case Status::BadChecksum: *os << "BadChecksum"; return;
    }
    *os << "Status(" << static_cast<int>(s) << ")";
}
void PrintTo(Decoder d, std::ostream* os) { *os << (d == Decoder::Table ? "Table" : "BitbyBit"); }
}  // namespace cd

namespace {

const std::vector<Status>& allStatuses() {
    static const std::vector<Status> s = {Status::Ok, Status::BadHeader, Status::BadLength,
                                          Status::BadCode, Status::Truncated, Status::BadChecksum};
    return s;
}

Bytes encoded(const Bytes& in) {
    Bytes out;
    EXPECT_EQ(cd::encode(in, out), Status::Ok);
    return out;
}

struct Result {
    Status status;
    Bytes out;
};

Result decoded(const Bytes& archive, Decoder how) {
    Result r;
    r.out = {0xEE, 0xEE};   // stale contents decode must get rid of
    r.status = cd::decode(archive, r.out, how);
    return r;
}

f::Header headerOf(const Bytes& archive) {
    f::Header h;
    EXPECT_TRUE(f::readHeader(archive.data(), archive.size(), h));
    return h;
}

Bytes payloadOf(const Bytes& archive) {
    return Bytes(archive.begin() + static_cast<std::ptrdiff_t>(f::HEADER_SIZE), archive.end());
}

bool isRaw(const Bytes& archive) { return (headerOf(archive).flags & f::FLAG_RAW) != 0; }

Bytes withPayload(const f::Header& h, const Bytes& payload) {
    Bytes a;
    f::writeHeader(h, a);
    a.insert(a.end(), payload.begin(), payload.end());
    return a;
}

// A Huffman archive built without the codec: any valid lengths, any data
// whose symbols all have codes.
Bytes handArchive(const Bytes& data, const hf::CodeLengths& len) {
    f::Header h;
    h.orig_len = data.size();
    h.crc = f::crc32(data);
    h.lengths = len;
    hf::CodeTable codes{};
    EXPECT_TRUE(hf::buildCanonicalCodes(len, codes));
    Bytes a;
    f::writeHeader(h, a);
    {
        BitWriter w(a);
        for (uint8_t b : data) w.write_bits(codes[b], len[b]);
    }
    return a;
}

uint64_t cost(const hf::FrequencyTable& freq, const hf::CodeLengths& len) {
    uint64_t c = 0;
    for (int s = 0; s < 256; ++s) c += freq[s] * len[s];
    return c;
}

void rewriteHeader(Bytes& archive, const f::Header& h) {
    Bytes fresh;
    f::writeHeader(h, fresh);
    std::copy(fresh.begin(), fresh.end(), archive.begin());
}

// Both decoders on one archive: same status, same output, Ok only with
// `original`, nothing left in the output on failure. Returns the status.
Status expectDecodersAgree(const Bytes& archive, const Bytes& original, const std::string& what) {
    const Result t = decoded(archive, Decoder::Table);
    const Result b = decoded(archive, Decoder::BitbyBit);
    EXPECT_EQ(t.status, b.status) << what;
    EXPECT_EQ(t.out, b.out) << what;
    if (t.status == Status::Ok) {
        EXPECT_EQ(t.out, original) << what << ": Ok with the wrong bytes";
    } else {
        EXPECT_TRUE(t.out.empty()) << what << ": output left behind on failure";
    }
    return t.status;
}

std::string decoderName(Decoder d) { return d == Decoder::Table ? "Table" : "BitByBit"; }

}  // namespace

// ===========================================================================
// messge
// ===========================================================================
TEST(MessgeTest, OkSaysOk) {
    EXPECT_STREQ(cd::messge(Status::Ok), "ok");
}

TEST(MessgeTest, EveryStatusHasItsOwnNonEmptyMessage) {
    std::set<std::string> seen;
    for (Status s : allStatuses()) {
        const char* m = cd::messge(s);
        ASSERT_NE(m, nullptr);
        EXPECT_GT(std::strlen(m), 1u);
        EXPECT_TRUE(seen.insert(m).second) << "duplicate message: " << m;
    }
}

TEST(MessgeTest, FailuresDoNotSayOk) {
    for (Status s : allStatuses()) {
        if (s != Status::Ok) EXPECT_STRNE(cd::messge(s), "ok");
    }
}

TEST(MessgeTest, UnknownStatusGetsAFallbackMessage) {
    const char* m = cd::messge(static_cast<Status>(99));
    ASSERT_NE(m, nullptr);
    EXPECT_GT(std::strlen(m), 0u);
    for (Status s : allStatuses()) EXPECT_STRNE(m, cd::messge(s));
}

// ===========================================================================
// encode: exact archives
// ===========================================================================
TEST(EncodeTest, EmptyInputIsAHeaderOnlyRawArchive) {
    const Bytes a = encoded(Bytes{});
    ASSERT_EQ(a.size(), f::HEADER_SIZE);
    const f::Header h = headerOf(a);
    EXPECT_EQ(h.flags, f::FLAG_RAW);
    EXPECT_EQ(h.orig_len, 0u);
    EXPECT_EQ(h.crc, 0u);
    EXPECT_EQ(h.lengths, hf::CodeLengths{});
}

TEST(EncodeTest, SingleByteIsStoredRaw) {
    for (int v = 0; v < 256; ++v) {
        const Bytes in = {static_cast<uint8_t>(v)};
        const Bytes a = encoded(in);
        ASSERT_EQ(a.size(), f::HEADER_SIZE + 1) << v;
        EXPECT_TRUE(isRaw(a));
        EXPECT_EQ(a.back(), v);
    }
}

TEST(EncodeTest, TwoDistinctBytesUseHuffman) {
    // a=0, b=1: two bits in one byte beat two bytes
    const Bytes a = encoded(ts::bytesOf("ab"));
    const f::Header h = headerOf(a);
    EXPECT_EQ(h.flags, 0);
    EXPECT_EQ(h.orig_len, 2u);
    EXPECT_EQ(h.lengths['a'], 1);
    EXPECT_EQ(h.lengths['b'], 1);
    EXPECT_EQ(payloadOf(a), (Bytes{0x40}));
}

TEST(EncodeTest, AlternatingAbGivesAlternatingBits) {
    std::string s;
    for (int i = 0; i < 8; ++i) s += "ab";
    EXPECT_EQ(payloadOf(encoded(ts::bytesOf(s))), (Bytes{0x55, 0x55}));
}

TEST(EncodeTest, AbracadabraGoldenArchive) {
    Bytes want = {'s', 'w', 'a', 's', 0x00, 11, 0, 0, 0, 0, 0, 0, 0, 0xB7, 0xF9, 0xEA, 0x17};
    want.resize(f::HEADER_SIZE, 0);
    want[17 + 'a'] = 1;
    want[17 + 'b'] = want[17 + 'c'] = want[17 + 'd'] = want[17 + 'r'] = 3;
    want.insert(want.end(), {0x4E, 0xAC, 0x9C});
    EXPECT_EQ(encoded(ts::bytesOf("abracadabra")), want);
}

TEST(EncodeTest, RunsOfOneByteAreOneBitPerByteOfZeros) {
    for (int n = 2; n <= 300; ++n) {
        const Bytes in(n, 'z');
        const Bytes a = encoded(in);
        const f::Header h = headerOf(a);
        ASSERT_EQ(h.flags, 0) << n;
        hf::CodeLengths want{};
        want['z'] = 1;
        ASSERT_EQ(h.lengths, want) << n;
        ASSERT_EQ(payloadOf(a), Bytes((n + 7) / 8, 0x00)) << n;
    }
}

TEST(EncodeTest, OutIsClearedFirst) {
    Bytes out(5000, 0xAB);
    ASSERT_EQ(cd::encode(ts::bytesOf("abracadabra"), out), Status::Ok);
    EXPECT_EQ(out, encoded(ts::bytesOf("abracadabra")));
    Bytes out2(5000, 0xAB);
    ASSERT_EQ(cd::encode(Bytes{}, out2), Status::Ok);
    EXPECT_EQ(out2.size(), f::HEADER_SIZE);
}

TEST(EncodeTest, ExactlyUniformBytesAreStoredRaw) {
    std::mt19937_64 rng(1);
    for (int k = 1; k <= 8; ++k) {
        Bytes in;
        for (int rep = 0; rep < k; ++rep)
            for (int s = 0; s < 256; ++s) in.push_back(static_cast<uint8_t>(s));
        std::shuffle(in.begin(), in.end(), rng);
        const Bytes a = encoded(in);
        EXPECT_TRUE(isRaw(a)) << "8-bit codes for all 256 values save nothing";
        EXPECT_EQ(payloadOf(a), in);
    }
}

TEST(EncodeTest, FibonacciDataUsesTheFullFifteenBits) {
    const Bytes in = ts::makeData(Shape::Fibonacci, 50000, 2);
    const Bytes a = encoded(in);
    const f::Header h = headerOf(a);
    ASSERT_EQ(h.flags, 0);
    EXPECT_EQ(hf::maxLength(h.lengths), hf::MAX_CODE_LEN);
    EXPECT_TRUE(hf::lengthsAreValid(h.lengths));
}

// ===========================================================================
// encode: the rules, over every shape and many sizes
// ===========================================================================
class EncodeShapeTest : public ::testing::TestWithParam<std::tuple<Shape, int>> {};

TEST_P(EncodeShapeTest, HeaderDescribesTheInput) {
    const Bytes in = ts::makeData(std::get<0>(GetParam()), std::get<1>(GetParam()), 3);
    const f::Header h = headerOf(encoded(in));
    EXPECT_EQ(h.orig_len, in.size());
    EXPECT_EQ(h.crc, f::crc32(in));
}

TEST_P(EncodeShapeTest, RawExactlyWhenHuffmanWouldNotBeSmaller) {
    const Bytes in = ts::makeData(std::get<0>(GetParam()), std::get<1>(GetParam()), 4);
    const auto freq = hf::countFrequencies(in);
    const auto len = hf::LimitCodeLengths(hf::buildCodeLengths(freq));
    const uint64_t huffBytes = (cost(freq, len) + 7) / 8;
    const Bytes a = encoded(in);

    if (huffBytes >= in.size()) {
        EXPECT_TRUE(isRaw(a));
        EXPECT_EQ(a.size(), f::HEADER_SIZE + in.size());
        EXPECT_EQ(payloadOf(a), in);
        EXPECT_EQ(headerOf(a).lengths, hf::CodeLengths{}) << "a raw archive carries no code table";
    } else {
        EXPECT_FALSE(isRaw(a));
        EXPECT_EQ(headerOf(a).lengths, len) << "the header holds the limited Huffman lengths";
        EXPECT_EQ(payloadOf(a).size(), huffBytes);
        EXPECT_LT(payloadOf(a).size(), in.size());
    }
}

TEST_P(EncodeShapeTest, NeverBiggerThanHeaderPlusInput) {
    const Bytes in = ts::makeData(std::get<0>(GetParam()), std::get<1>(GetParam()), 5);
    EXPECT_LE(encoded(in).size(), f::HEADER_SIZE + in.size());
}

TEST_P(EncodeShapeTest, HuffmanPayloadIsTheCanonicalCodesInOrderWithZeroPadding) {
    const Bytes in = ts::makeData(std::get<0>(GetParam()), std::get<1>(GetParam()), 6);
    const Bytes a = encoded(in);
    if (isRaw(a)) return;
    const f::Header h = headerOf(a);
    EXPECT_EQ(a, handArchive(in, h.lengths));
}

TEST_P(EncodeShapeTest, IsDeterministic) {
    const Bytes in = ts::makeData(std::get<0>(GetParam()), std::get<1>(GetParam()), 7);
    EXPECT_EQ(encoded(in), encoded(in));
}

TEST_P(EncodeShapeTest, ArchiveAlwaysParses) {
    const Bytes in = ts::makeData(std::get<0>(GetParam()), std::get<1>(GetParam()), 8);
    const Bytes a = encoded(in);
    f::Header h;
    EXPECT_TRUE(f::readHeader(a.data(), a.size(), h));
}

INSTANTIATE_TEST_SUITE_P(ShapesAndSizes, EncodeShapeTest,
                         ::testing::Combine(::testing::ValuesIn(ts::allShapes()),
                                            ::testing::Values(0, 1, 2, 3, 8, 9, 64, 255, 256, 1000, 4096, 65536)),
                         [](const auto& info) {
                             return std::string(ts::shapeName(std::get<0>(info.param))) + "_" +
                                    std::to_string(std::get<1>(info.param));
                         });

// ===========================================================================
// decode, for each decoder
// ===========================================================================
class DecodeTest : public ::testing::TestWithParam<Decoder> {
protected:
    Result dec(const Bytes& archive) { return decoded(archive, GetParam()); }
};

TEST_P(DecodeTest, AbracadabraRoundTrips) {
    const Result r = dec(encoded(ts::bytesOf("abracadabra")));
    EXPECT_EQ(r.status, Status::Ok);
    EXPECT_EQ(r.out, ts::bytesOf("abracadabra"));
}

TEST_P(DecodeTest, EmptyRawArchiveDecodesToNothing) {
    const Result r = dec(encoded(Bytes{}));
    EXPECT_EQ(r.status, Status::Ok);
    EXPECT_TRUE(r.out.empty());
}

TEST_P(DecodeTest, EmptyHuffmanArchiveDecodesToNothing) {
    // not something encode writes, but a valid archive: no bytes, no codes, crc of nothing
    const Result r = dec(withPayload(f::Header{}, {}));
    EXPECT_EQ(r.status, Status::Ok);
    EXPECT_TRUE(r.out.empty());
}

TEST_P(DecodeTest, EmptyHuffmanArchiveWithAPayloadByteIsBadLength) {
    EXPECT_EQ(dec(withPayload(f::Header{}, {0x00})).status, Status::BadLength);
}

TEST_P(DecodeTest, HandBuiltRawArchiveReturnsItsPayload) {
    const Bytes data = ts::bytesOf("stored as is");
    f::Header h;
    h.flags = f::FLAG_RAW;
    h.orig_len = data.size();
    h.crc = f::crc32(data);
    const Result r = dec(withPayload(h, data));
    EXPECT_EQ(r.status, Status::Ok);
    EXPECT_EQ(r.out, data);
}

TEST_P(DecodeTest, StaleOutputIsReplacedOnSuccess) {
    Bytes out(1000, 0x77);
    ASSERT_EQ(cd::decode(encoded(ts::bytesOf("hello hello")), out, GetParam()), Status::Ok);
    EXPECT_EQ(out, ts::bytesOf("hello hello"));
}

TEST_P(DecodeTest, CodesOfOneLengthOnly) {
    // two symbols with the same code length, for every length 1..15: lengths
    // up to 9 go through the fast table, 10 and up through the long path
    for (int L = 1; L <= hf::MAX_CODE_LEN; ++L) {
        hf::CodeLengths len{};
        len['x'] = static_cast<uint8_t>(L);
        len['y'] = static_cast<uint8_t>(L);
        std::mt19937_64 rng(L);
        Bytes data;
        for (int i = 0; i < 500; ++i) data.push_back(rng() & 1 ? 'x' : 'y');
        const Result r = dec(handArchive(data, len));
        EXPECT_EQ(r.status, Status::Ok) << "length " << L;
        EXPECT_EQ(r.out, data) << "length " << L;
    }
}

TEST_P(DecodeTest, ChainCodeCoversEveryLength) {
    // lengths 1, 2, ..., 14, 15, 15: complete, one code of every length
    hf::CodeLengths len{};
    for (int l = 1; l <= 14; ++l) len[10 * l] = static_cast<uint8_t>(l);
    len[200] = len[201] = 15;
    std::vector<uint8_t> symbols;
    for (int s = 0; s < 256; ++s)
        if (len[s]) symbols.push_back(static_cast<uint8_t>(s));
    std::mt19937_64 rng(9);
    Bytes data;
    for (int i = 0; i < 3000; ++i) data.push_back(symbols[rng() % symbols.size()]);
    const Result r = dec(handArchive(data, len));
    EXPECT_EQ(r.status, Status::Ok);
    EXPECT_EQ(r.out, data);
}

TEST_P(DecodeTest, EverySymbolAtFifteenBitsGoesThroughTheLongPath) {
    hf::CodeLengths len;
    len.fill(15);
    Bytes data;
    for (int rep = 0; rep < 4; ++rep)
        for (int s = 0; s < 256; ++s) data.push_back(static_cast<uint8_t>(s));
    std::mt19937_64 rng(10);
    std::shuffle(data.begin(), data.end(), rng);
    const Result r = dec(handArchive(data, len));
    EXPECT_EQ(r.status, Status::Ok);
    EXPECT_EQ(r.out, data);
}

TEST_P(DecodeTest, CodesStraddlingTheNineBitFastTable) {
    // 128 symbols at 8 bits, 64 at 9, 64 at 10: an incomplete code on both sides of the boundary
    hf::CodeLengths len{};
    for (int s = 0; s < 256; ++s) len[s] = s < 128 ? 8 : s < 192 ? 9 : 10;
    ASSERT_TRUE(hf::lengthsAreValid(len));
    Bytes data;
    for (int rep = 0; rep < 3; ++rep)
        for (int s = 255; s >= 0; --s) data.push_back(static_cast<uint8_t>(s));
    const Result r = dec(handArchive(data, len));
    EXPECT_EQ(r.status, Status::Ok);
    EXPECT_EQ(r.out, data);
}

TEST_P(DecodeTest, LimitedFibonacciLengthsDecode) {
    const Bytes data = ts::makeData(Shape::Fibonacci, 30000, 11);
    const Bytes a = encoded(data);
    ASSERT_FALSE(isRaw(a));
    ASSERT_EQ(hf::maxLength(headerOf(a).lengths), 15);
    const Result r = dec(a);
    EXPECT_EQ(r.status, Status::Ok);
    EXPECT_EQ(r.out, data);
}

TEST_P(DecodeTest, QuarterMebibyteOfTextRoundTrips) {
    const Bytes data = ts::makeData(Shape::Text, 1 << 18, 12);
    const Result r = dec(encoded(data));
    EXPECT_EQ(r.status, Status::Ok);
    EXPECT_TRUE(r.out == data);
}

// ---------------------------------------------------------------------------
// BadHeader
// ---------------------------------------------------------------------------
TEST_P(DecodeTest, EmptyInputIsBadHeader) {
    EXPECT_EQ(dec(Bytes{}).status, Status::BadHeader);
}

TEST_P(DecodeTest, EveryInputShorterThanAHeaderIsBadHeader) {
    const Bytes a = encoded(ts::bytesOf("abracadabra"));
    for (std::size_t n = 0; n < f::HEADER_SIZE; ++n) {
        ASSERT_EQ(dec(Bytes(a.begin(), a.begin() + n)).status, Status::BadHeader) << n;
    }
}

TEST_P(DecodeTest, NotAnArchiveIsBadHeader) {
    EXPECT_EQ(dec(ts::bytesOf(std::string(400, 'x'))).status, Status::BadHeader);
    Bytes a = encoded(ts::bytesOf("abracadabra"));
    a[0] = 'S';
    EXPECT_EQ(dec(a).status, Status::BadHeader);
}

TEST_P(DecodeTest, UnknownFlagIsBadHeader) {
    Bytes a = encoded(ts::bytesOf("abracadabra"));
    for (int bit = 1; bit < 8; ++bit) {
        Bytes b = a;
        b[4] |= static_cast<uint8_t>(1u << bit);
        EXPECT_EQ(dec(b).status, Status::BadHeader) << "flag bit " << bit;
    }
}

TEST_P(DecodeTest, BadLengthsInTheHeaderAreBadHeader) {
    Bytes a = encoded(ts::bytesOf("abracadabra"));
    Bytes tooLong = a;
    tooLong[17 + 'a'] = 16;
    EXPECT_EQ(dec(tooLong).status, Status::BadHeader);
    Bytes overfull = a;
    overfull[17 + 'z'] = 1;   // a second 1-bit code next to 'a' and four 3-bit ones
    EXPECT_EQ(dec(overfull).status, Status::BadHeader);
}

TEST_P(DecodeTest, RawFlagWithACodeTableIsBadHeader) {
    Bytes a = encoded(ts::bytesOf("abracadabra"));
    a[4] = f::FLAG_RAW;
    EXPECT_EQ(dec(a).status, Status::BadHeader);
}

// ---------------------------------------------------------------------------
// BadLength
// ---------------------------------------------------------------------------
TEST_P(DecodeTest, RawLengthMismatchIsBadLength) {
    const Bytes data = ts::bytesOf("x");
    Bytes a = encoded(data);
    ASSERT_TRUE(isRaw(a));
    for (uint64_t n : {0ull, 2ull, 100ull, ~0ull}) {
        f::Header h = headerOf(a);
        h.orig_len = n;
        Bytes b = a;
        rewriteHeader(b, h);
        EXPECT_EQ(dec(b).status, Status::BadLength) << n;
    }
}

TEST_P(DecodeTest, MoreSymbolsThanPayloadBitsIsBadLength) {
    Bytes a = encoded(ts::bytesOf("abracadabra"));   // 3 payload bytes, 24 bits
    f::Header h = headerOf(a);
    for (uint64_t n : {25ull, 1000ull, 1ull << 40, ~0ull}) {
        h.orig_len = n;
        rewriteHeader(a, h);
        EXPECT_EQ(dec(a).status, Status::BadLength) << n;
    }
}

TEST_P(DecodeTest, TrailingPayloadBytesAreBadLength) {
    for (const Bytes& data : {ts::bytesOf("abracadabra"), ts::bytesOf("x"), Bytes{}}) {
        Bytes a = encoded(data);
        for (int extra = 1; extra <= 8; ++extra) {
            a.push_back(0);
            EXPECT_EQ(dec(a).status, Status::BadLength) << extra << " extra bytes, raw " << isRaw(a);
        }
    }
}

TEST_P(DecodeTest, FewerSymbolsThanThePayloadHoldsIsBadLength) {
    Bytes a = encoded(ts::bytesOf("abracadabra"));
    f::Header h = headerOf(a);
    h.orig_len = 8;   // "abracada" is 16 bits: two bytes of a three-byte payload
    rewriteHeader(a, h);
    EXPECT_EQ(dec(a).status, Status::BadLength);
}

// ---------------------------------------------------------------------------
// BadCode and Truncated, from an incomplete code: a = 0, b = 10, 11... is no code
// ---------------------------------------------------------------------------
TEST_P(DecodeTest, UnusedCodeWithFifteenBitsLeftIsBadCode) {
    f::Header h;
    h.orig_len = 1;
    h.lengths['a'] = 1;
    h.lengths['b'] = 2;
    EXPECT_EQ(dec(withPayload(h, {0xC0, 0x00})).status, Status::BadCode);
    EXPECT_EQ(dec(withPayload(h, {0xFF, 0xFF})).status, Status::BadCode);
}

TEST_P(DecodeTest, UnusedCodeAfterValidSymbolsIsBadCode) {
    f::Header h;
    h.orig_len = 5;
    h.lengths['a'] = 1;
    h.lengths['b'] = 2;
    // a a b then 11: 0 0 10 11 000 00000000 00000000
    EXPECT_EQ(dec(withPayload(h, {0x2C, 0x00, 0x00})).status, Status::BadCode);
}

TEST_P(DecodeTest, UnusedCodeNearTheEndIsTruncated) {
    f::Header h;
    h.orig_len = 1;
    h.lengths['a'] = 1;
    h.lengths['b'] = 2;
    EXPECT_EQ(dec(withPayload(h, {0xC0})).status, Status::Truncated);
}

TEST_P(DecodeTest, OneSymbolTooManyIsTruncatedOrBadChecksum) {
    // abracadabra is 23 bits in 3 bytes. A 12th symbol reads the zero pad bit
    // as 'a' (code 0) and fails the crc; a 13th runs off the end.
    Bytes a = encoded(ts::bytesOf("abracadabra"));
    f::Header h = headerOf(a);
    h.orig_len = 12;
    rewriteHeader(a, h);
    EXPECT_EQ(dec(a).status, Status::BadChecksum);
    h.orig_len = 13;
    rewriteHeader(a, h);
    EXPECT_EQ(dec(a).status, Status::Truncated);
}

TEST_P(DecodeTest, DroppingPayloadBytesIsTruncatedOrBadLength) {
    const Bytes data = ts::makeData(Shape::Text, 1500, 13);
    const Bytes a = encoded(data);
    ASSERT_FALSE(isRaw(a));
    for (std::size_t drop = 1; drop <= payloadOf(a).size(); ++drop) {
        const Status s = dec(Bytes(a.begin(), a.end() - static_cast<std::ptrdiff_t>(drop))).status;
        ASSERT_TRUE(s == Status::Truncated || s == Status::BadLength) << "dropped " << drop;
    }
}

// ---------------------------------------------------------------------------
// BadChecksum
// ---------------------------------------------------------------------------
TEST_P(DecodeTest, WrongCrcIsBadChecksum) {
    for (const Bytes& data : {ts::bytesOf("abracadabra"), ts::bytesOf("x"), Bytes{}}) {
        const Bytes a = encoded(data);
        for (int bit = 0; bit < 32; ++bit) {
            f::Header h = headerOf(a);
            h.crc ^= 1u << bit;
            Bytes b = a;
            rewriteHeader(b, h);
            EXPECT_EQ(dec(b).status, Status::BadChecksum) << "crc bit " << bit << ", raw " << isRaw(a);
        }
    }
}

TEST_P(DecodeTest, EveryFlippedRawPayloadBitIsBadChecksum) {
    std::mt19937_64 rng(14);
    Bytes data;   // every byte value exactly once: Huffman cannot win, so it is stored raw
    for (int s = 0; s < 256; ++s) data.push_back(static_cast<uint8_t>(s));
    std::shuffle(data.begin(), data.end(), rng);
    const Bytes a = encoded(data);
    ASSERT_TRUE(isRaw(a));
    for (std::size_t bit = f::HEADER_SIZE * 8; bit < a.size() * 8; ++bit) {
        Bytes b = a;
        b[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
        ASSERT_EQ(dec(b).status, Status::BadChecksum) << "bit " << bit;
    }
}

// ---------------------------------------------------------------------------
// Failure leaves nothing behind
// ---------------------------------------------------------------------------
TEST_P(DecodeTest, OutputIsEmptyAfterEveryKindOfFailure) {
    Bytes good = encoded(ts::bytesOf("abracadabra"));
    f::Header h = headerOf(good);

    Bytes badChecksum = good;
    h.crc ^= 1;
    rewriteHeader(badChecksum, h);

    Bytes truncated = good;
    h = headerOf(good);
    h.orig_len = 13;
    rewriteHeader(truncated, h);

    f::Header inc;
    inc.orig_len = 1;
    inc.lengths['a'] = 1;
    inc.lengths['b'] = 2;

    Bytes trailing = good;
    trailing.push_back(0x00);

    const std::vector<std::pair<Bytes, Status>> cases = {
        {Bytes(10, 0), Status::BadHeader},
        {trailing, Status::BadLength},
        {badChecksum, Status::BadChecksum},
        {truncated, Status::Truncated},
        {withPayload(inc, {0xC0, 0x00}), Status::BadCode},
    };
    for (const auto& c : cases) {
        Bytes out(77, 0x42);
        EXPECT_EQ(cd::decode(c.first, out, GetParam()), c.second);
        EXPECT_TRUE(out.empty());
    }
}

INSTANTIATE_TEST_SUITE_P(BothDecoders, DecodeTest, ::testing::Values(Decoder::Table, Decoder::BitbyBit),
                         [](const auto& info) { return decoderName(info.param); });

TEST(DecodeDefaultTest, DefaultDecoderIsTheTable) {
    const Bytes a = encoded(ts::makeData(Shape::Geometric, 5000, 15));
    Bytes byDefault, byTable;
    EXPECT_EQ(cd::decode(a, byDefault), cd::decode(a, byTable, Decoder::Table));
    EXPECT_EQ(byDefault, byTable);
}

// ===========================================================================
// Round trips: every shape x size x decoder
// ===========================================================================
class RoundTripTest : public ::testing::TestWithParam<std::tuple<Shape, int, Decoder>> {};

TEST_P(RoundTripTest, DecodeOfEncodeIsTheInput) {
    const Bytes in = ts::makeData(std::get<0>(GetParam()), std::get<1>(GetParam()), 16);
    const Result r = decoded(encoded(in), std::get<2>(GetParam()));
    ASSERT_EQ(r.status, Status::Ok);
    EXPECT_TRUE(r.out == in);
}

INSTANTIATE_TEST_SUITE_P(ShapesSizesDecoders, RoundTripTest,
                         ::testing::Combine(::testing::ValuesIn(ts::allShapes()),
                                            ::testing::Values(0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 100, 255, 256, 257,
                                                              1000, 4096, 50000),
                                            ::testing::Values(Decoder::Table, Decoder::BitbyBit)),
                         [](const auto& info) {
                             return std::string(ts::shapeName(std::get<0>(info.param))) + "_" +
                                    std::to_string(std::get<1>(info.param)) + "_" +
                                    decoderName(std::get<2>(info.param));
                         });

// Random shape, random size, both decoders: 300 seeds.
class RandomRoundTripTest : public ::testing::TestWithParam<int> {};

TEST_P(RandomRoundTripTest, BothDecodersReturnTheInput) {
    std::mt19937_64 rng(static_cast<uint64_t>(GetParam()) * 6364136223846793005ull + 1);
    const Shape shape = ts::allShapes()[rng() % ts::allShapes().size()];
    const Bytes in = ts::makeData(shape, rng() % 3000, rng());
    const Bytes a = encoded(in);
    for (Decoder d : {Decoder::Table, Decoder::BitbyBit}) {
        const Result r = decoded(a, d);
        ASSERT_EQ(r.status, Status::Ok) << ts::shapeName(shape) << " " << in.size() << " " << decoderName(d);
        ASSERT_TRUE(r.out == in) << ts::shapeName(shape) << " " << in.size() << " " << decoderName(d);
    }
}

INSTANTIATE_TEST_SUITE_P(Seeds, RandomRoundTripTest, ::testing::Range(0, 300));

// ===========================================================================
// Robustness: every truncation, every bit flip, random garbage
// ===========================================================================
class CorruptionTest : public ::testing::TestWithParam<std::tuple<Shape, int>> {
protected:
    Bytes data() const { return ts::makeData(std::get<0>(GetParam()), std::get<1>(GetParam()), 17); }
};

TEST_P(CorruptionTest, EveryTruncationFailsTheSameWayInBothDecoders) {
    const Bytes in = data();
    const Bytes a = encoded(in);
    for (std::size_t n = 0; n < a.size(); ++n) {
        const Bytes cut(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(n));
        const Status s = decoded(cut, Decoder::Table).status;
        ASSERT_TRUE(s == Status::BadHeader || s == Status::BadLength || s == Status::Truncated)
            << "cut at " << n << " of " << a.size();
        expectDecodersAgree(cut, in, "cut at " + std::to_string(n));
        if (::testing::Test::HasFailure()) return;
    }
}

TEST_P(CorruptionTest, EveryBitFlipIsCaughtOrHarmless) {
    const Bytes in = data();
    const Bytes a = encoded(in);
    const std::size_t fixedFields = 17 * 8;   // magic, flags, orig_len, crc: a flip there is always caught
    for (std::size_t bit = 0; bit < a.size() * 8; ++bit) {
        Bytes b = a;
        b[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
        const Status s = expectDecodersAgree(b, in, "bit " + std::to_string(bit));
        if (bit < fixedFields) ASSERT_NE(s, Status::Ok) << "header bit " << bit;
        if (::testing::Test::HasFailure()) return;
    }
}

TEST_P(CorruptionTest, AppendedBytesAreAlwaysBadLength) {
    const Bytes in = data();
    Bytes a = encoded(in);
    std::mt19937_64 rng(18);
    for (int extra = 0; extra < 20; ++extra) {
        a.push_back(static_cast<uint8_t>(rng()));
        ASSERT_EQ(decoded(a, Decoder::Table).status, Status::BadLength) << extra;
        ASSERT_EQ(decoded(a, Decoder::BitbyBit).status, Status::BadLength) << extra;
    }
}

INSTANTIATE_TEST_SUITE_P(ShapesAndSizes, CorruptionTest,
                         ::testing::Combine(::testing::ValuesIn(ts::allShapes()), ::testing::Values(1, 12, 100)),
                         [](const auto& info) {
                             return std::string(ts::shapeName(std::get<0>(info.param))) + "_" +
                                    std::to_string(std::get<1>(info.param));
                         });

// Random valid code tables with random payloads: nothing to decode correctly,
// only a status to agree on. 300 seeds.
class GarbagePayloadTest : public ::testing::TestWithParam<int> {};

TEST_P(GarbagePayloadTest, DecodersAgreeOnRandomPayloads) {
    std::mt19937_64 rng(static_cast<uint64_t>(GetParam()) * 1442695040888963407ull + 19);
    for (int t = 0; t < 10; ++t) {
        hf::FrequencyTable freq{};
        const int k = 1 + static_cast<int>(rng() % 60);
        for (int i = 0; i < k; ++i) freq[rng() % 256] = 1 + (rng() >> (20 + rng() % 44));
        hf::CodeLengths len = hf::LimitCodeLengths(hf::buildCodeLengths(freq));
        if (rng() % 2) {   // punch holes: an incomplete code has bit patterns that are no code
            for (int drop = 0; drop < 3; ++drop) len[rng() % 256] = 0;
            if (std::none_of(len.begin(), len.end(), [](uint8_t l) { return l != 0; })) len[0] = 1;
        }
        const Bytes payload = ts::randomBytes(rng, rng() % 64);
        f::Header h;
        h.lengths = len;
        h.orig_len = payload.empty() ? 0 : rng() % (payload.size() * 8 + 2);
        h.crc = static_cast<uint32_t>(rng());
        const Bytes a = withPayload(h, payload);

        const Result tb = decoded(a, Decoder::Table);
        const Result bb = decoded(a, Decoder::BitbyBit);
        ASSERT_EQ(tb.status, bb.status) << "trial " << t;
        ASSERT_EQ(tb.out, bb.out) << "trial " << t;
        if (tb.status == Status::Ok) {
            ASSERT_EQ(tb.out.size(), h.orig_len);
            ASSERT_EQ(f::crc32(tb.out), h.crc);
        }
    }
}

TEST_P(GarbagePayloadTest, AnyPayloadDecodesUnderACompleteCode) {
    // Under a complete code every bit string is a run of codes. Walk random
    // bytes with a hand decoder that only knows the code table, cut the
    // payload after the last whole symbol, and give the header that count and
    // crc: both decoders must then return exactly what the hand decoder found.
    std::mt19937_64 rng(static_cast<uint64_t>(GetParam()) * 22695477 + 20);
    hf::FrequencyTable freq{};
    freq[7] = 1 + rng() % 5000;   // two symbols at least, so the code is complete
    freq[200] = 1 + rng() % 5000;
    const int k = static_cast<int>(rng() % 100);
    for (int i = 0; i < k; ++i) freq[rng() % 256] = 1 + (rng() >> (40 + rng() % 24));
    f::Header h;
    h.lengths = hf::LimitCodeLengths(hf::buildCodeLengths(freq));
    hf::CodeTable codes{};
    ASSERT_TRUE(hf::buildCanonicalCodes(h.lengths, codes));

    std::map<std::pair<int, uint32_t>, uint8_t> lookup;
    for (int s = 0; s < 256; ++s)
        if (h.lengths[s]) lookup[{h.lengths[s], codes[s]}] = static_cast<uint8_t>(s);

    Bytes payload = ts::randomBytes(rng, 1 + rng() % 200);
    const uint64_t total = payload.size() * 8;
    auto bitAt = [&](uint64_t p) { return static_cast<uint32_t>((payload[p / 8] >> (7 - p % 8)) & 1); };

    Bytes want;
    uint64_t pos = 0;
    for (;;) {
        uint32_t code = 0;
        int len = 0;
        auto it = lookup.end();
        while (it == lookup.end() && len < hf::MAX_CODE_LEN && pos + len < total) {
            code = (code << 1) | bitAt(pos + len);
            ++len;
            it = lookup.find({len, code});
        }
        if (it == lookup.end()) break;   // the bits ran out mid-code
        want.push_back(it->second);
        pos += len;
    }
    if (want.empty()) GTEST_SKIP() << "payload shorter than its first code";

    payload.resize((pos + 7) / 8);   // no whole byte left unread
    h.orig_len = want.size();
    h.crc = f::crc32(want);
    const Bytes a = withPayload(h, payload);
    for (Decoder d : {Decoder::Table, Decoder::BitbyBit}) {
        const Result r = decoded(a, d);
        EXPECT_EQ(r.status, Status::Ok) << decoderName(d);
        EXPECT_EQ(r.out, want) << decoderName(d);
    }
}

INSTANTIATE_TEST_SUITE_P(Seeds, GarbagePayloadTest, ::testing::Range(0, 300));
