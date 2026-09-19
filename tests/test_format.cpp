// test_format.cpp -- f::, the archive header and CRC-32.
//
// writeHeader is checked byte by byte against the documented layout.
// readHeader is checked against an independent validator written from the
// same rules, over every short size, every flag value, every bad magic byte,
// every overlong length and thousands of random mutations; on failure it must
// leave the caller's header untouched. crc32 is checked against zlib's
// published values, a bitwise reference, and the error-detection guarantees
// CRC-32 makes (every 1- and 2-bit error, every burst up to 32 bits).
#include "Format.hpp"
#include "Huffman.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using ts::Bytes;

namespace {

// Bitwise CRC-32 straight from the reflected polynomial: no table, no tricks.
uint32_t referenceCrc(const uint8_t* p, std::size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
uint32_t referenceCrc(const Bytes& b) { return referenceCrc(b.data(), b.size()); }

Bytes headerBytes(const f::Header& h) {
    Bytes out;
    f::writeHeader(h, out);
    return out;
}

uint64_t le(const Bytes& b, std::size_t at, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v |= static_cast<uint64_t>(b[at + i]) << (8 * i);
    return v;
}

void expectSameHeader(const f::Header& a, const f::Header& b) {
    EXPECT_EQ(a.flags, b.flags);
    EXPECT_EQ(a.orig_len, b.orig_len);
    EXPECT_EQ(a.crc, b.crc);
    EXPECT_EQ(a.lengths, b.lengths);
}

bool kraftOk(const hf::CodeLengths& len) {
    uint64_t k = 0;
    for (uint8_t l : len) {
        if (l > 15) return false;
        if (l) k += 1ull << (15 - l);
    }
    return k <= (1ull << 15);
}

// The acceptance rules, written from the header layout comment and not from Format.cpp.
bool specAccepts(const Bytes& b) {
    if (b.size() < f::HEADER_SIZE) return false;
    if (std::memcmp(b.data(), "swas", 4) != 0) return false;
    const uint8_t flags = b[4];
    if (flags != 0 && flags != f::FLAG_RAW) return false;
    const uint64_t origLen = le(b, 5, 8);
    hf::CodeLengths len{};
    bool any = false;
    for (int s = 0; s < 256; ++s) {
        len[s] = b[17 + s];
        any |= len[s] != 0;
    }
    if (!kraftOk(len)) return false;
    if (flags == f::FLAG_RAW) return !any;
    return origLen == 0 || any;
}

f::Header randomValidHeader(std::mt19937_64& rng) {
    f::Header h;
    h.crc = static_cast<uint32_t>(rng());
    h.orig_len = rng() >> (rng() % 64);
    if (rng() % 3 == 0) {
        h.flags = f::FLAG_RAW;
        return h;
    }
    hf::FrequencyTable freq{};
    const int k = 1 + static_cast<int>(rng() % 256);
    for (int i = 0; i < k; ++i) freq[rng() % 256] = 1 + (rng() >> (12 + rng() % 52));   // total < 2^60
    h.lengths = hf::LimitCodeLengths(hf::buildCodeLengths(freq));
    return h;
}

f::Header sentinel() {
    f::Header h;
    h.flags = 0x5A;
    h.orig_len = 0x1122334455667788ull;
    h.crc = 0xCAFEBABE;
    for (int s = 0; s < 256; ++s) h.lengths[s] = static_cast<uint8_t>(200 + s % 50);
    return h;
}

// Expects readHeader to refuse `b` and leave a pre-filled header as it was.
void expectRejected(const Bytes& b, const std::string& why) {
    f::Header h = sentinel();
    EXPECT_FALSE(f::readHeader(b.data(), b.size(), h)) << why;
    const f::Header s = sentinel();
    EXPECT_EQ(h.flags, s.flags) << why;
    EXPECT_EQ(h.orig_len, s.orig_len) << why;
    EXPECT_EQ(h.crc, s.crc) << why;
    EXPECT_EQ(h.lengths, s.lengths) << why;
}

}  // namespace

// ===========================================================================
// Constants
// ===========================================================================
TEST(FormatConstantsTest, HeaderSizeIs273) {
    EXPECT_EQ(f::HEADER_SIZE, 273u);
}

TEST(FormatConstantsTest, HeaderSizeIsTheSumOfTheFields) {
    EXPECT_EQ(f::HEADER_SIZE, sizeof(f::MAGIC) + sizeof(f::Header::flags) + sizeof(f::Header::orig_len) +
                                  sizeof(f::Header::crc) + sizeof(f::Header::lengths));
}

TEST(FormatConstantsTest, FlagRawIsBitZero) {
    EXPECT_EQ(f::FLAG_RAW, 0x01);
}

TEST(FormatConstantsTest, MagicSpellsSwas) {
    EXPECT_EQ(std::string(f::MAGIC, f::MAGIC + 4), "swas");
}

TEST(FormatConstantsTest, DefaultHeaderIsAllZero) {
    const f::Header h;
    EXPECT_EQ(h.flags, 0);
    EXPECT_EQ(h.orig_len, 0u);
    EXPECT_EQ(h.crc, 0u);
    EXPECT_EQ(h.lengths, hf::CodeLengths{});
}

// ===========================================================================
// writeHeader
// ===========================================================================
TEST(WriteHeaderTest, DefaultHeaderIsMagicThenZeros) {
    const Bytes b = headerBytes(f::Header{});
    ASSERT_EQ(b.size(), f::HEADER_SIZE);
    EXPECT_EQ(Bytes(b.begin(), b.begin() + 4), ts::bytesOf("swas"));
    for (std::size_t i = 4; i < b.size(); ++i) EXPECT_EQ(b[i], 0) << "byte " << i;
}

TEST(WriteHeaderTest, AppendsWithoutClearing) {
    Bytes out = {1, 2, 3};
    f::writeHeader(f::Header{}, out);
    ASSERT_EQ(out.size(), 3 + f::HEADER_SIZE);
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[2], 3);
    EXPECT_EQ(out[3], 's');
}

TEST(WriteHeaderTest, TwoHeadersBackToBackBothParse) {
    std::mt19937_64 rng(1);
    const f::Header a = randomValidHeader(rng), b = randomValidHeader(rng);
    Bytes out;
    f::writeHeader(a, out);
    f::writeHeader(b, out);
    ASSERT_EQ(out.size(), 2 * f::HEADER_SIZE);
    f::Header ra, rb;
    ASSERT_TRUE(f::readHeader(out.data(), out.size(), ra));
    ASSERT_TRUE(f::readHeader(out.data() + f::HEADER_SIZE, f::HEADER_SIZE, rb));
    expectSameHeader(ra, a);
    expectSameHeader(rb, b);
}

TEST(WriteHeaderTest, FieldsSitAtTheDocumentedOffsets) {
    f::Header h;
    h.flags = f::FLAG_RAW;
    h.orig_len = 0x0807060504030201ull;
    h.crc = 0x0D0C0B0Au;
    for (int s = 0; s < 256; ++s) h.lengths[s] = static_cast<uint8_t>(s % 16);
    const Bytes b = headerBytes(h);
    ASSERT_EQ(b.size(), f::HEADER_SIZE);
    EXPECT_EQ(b[4], 0x01);
    for (int i = 0; i < 8; ++i) EXPECT_EQ(b[5 + i], i + 1) << "orig_len byte " << i;
    for (int i = 0; i < 4; ++i) EXPECT_EQ(b[13 + i], 0x0A + i) << "crc byte " << i;
    for (int s = 0; s < 256; ++s) EXPECT_EQ(b[17 + s], s % 16) << "length " << s;
}

TEST(WriteHeaderTest, AbracadabraGoldenHeader) {
    f::Header h;
    h.orig_len = 11;
    h.crc = 0x17EAF9B7;   // zlib.crc32(b"abracadabra")
    h.lengths['a'] = 1;
    h.lengths['b'] = h.lengths['c'] = h.lengths['d'] = h.lengths['r'] = 3;
    const Bytes b = headerBytes(h);

    Bytes want = {'s', 'w', 'a', 's', 0x00, 11, 0, 0, 0, 0, 0, 0, 0, 0xB7, 0xF9, 0xEA, 0x17};
    want.resize(f::HEADER_SIZE, 0);
    want[17 + 'a'] = 1;
    want[17 + 'b'] = want[17 + 'c'] = want[17 + 'd'] = want[17 + 'r'] = 3;
    EXPECT_EQ(b, want);
}

TEST(WriteHeaderTest, FlagsByteIsWrittenAsIs) {
    for (int v = 0; v < 256; ++v) {
        f::Header h;
        h.flags = static_cast<uint8_t>(v);
        EXPECT_EQ(headerBytes(h)[4], v);
    }
}

TEST(WriteHeaderTest, CrcIsLittleEndian) {
    for (uint32_t c : {0u, 1u, 0xFFu, 0x100u, 0xDEADBEEFu, 0xFFFFFFFFu, 0x80000000u}) {
        f::Header h;
        h.crc = c;
        EXPECT_EQ(le(headerBytes(h), 13, 4), c) << std::hex << c;
    }
}

class OrigLenEncodingTest : public ::testing::TestWithParam<uint64_t> {};

TEST_P(OrigLenEncodingTest, IsLittleEndianAndReadsBack) {
    f::Header h;
    h.flags = f::FLAG_RAW;
    h.orig_len = GetParam();
    const Bytes b = headerBytes(h);
    EXPECT_EQ(le(b, 5, 8), GetParam());
    for (int i = 0; i < 8; ++i) EXPECT_EQ(b[5 + i], static_cast<uint8_t>(GetParam() >> (8 * i))) << "byte " << i;
    f::Header r;
    ASSERT_TRUE(f::readHeader(b.data(), b.size(), r));
    EXPECT_EQ(r.orig_len, GetParam());
}

INSTANTIATE_TEST_SUITE_P(Values, OrigLenEncodingTest,
                         ::testing::Values(0ull, 1ull, 0xFFull, 0x100ull, 0xFFFFull, 0x10000ull, 0xFFFFFFFFull,
                                           0x100000000ull, 0x0102030405060708ull, 0x8000000000000000ull,
                                           0x7FFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, 1ull << 40,
                                           123456789012345ull));

// ===========================================================================
// readHeader: round trips
// ===========================================================================
TEST(ReadHeaderTest, DefaultHeaderRoundTrips) {
    const Bytes b = headerBytes(f::Header{});
    f::Header r = sentinel();
    ASSERT_TRUE(f::readHeader(b.data(), b.size(), r)) << "huffman flag, nothing to decode, no codes: an empty file";
    expectSameHeader(r, f::Header{});
}

TEST(ReadHeaderTest, TrailingBytesAreIgnored) {
    std::mt19937_64 rng(2);
    const f::Header h = randomValidHeader(rng);
    Bytes b = headerBytes(h);
    const Bytes payload = ts::randomBytes(rng, 1000);
    b.insert(b.end(), payload.begin(), payload.end());
    f::Header r;
    ASSERT_TRUE(f::readHeader(b.data(), b.size(), r));
    expectSameHeader(r, h);
}

TEST(ReadHeaderTest, SuccessOverwritesEveryField) {
    f::Header h;
    h.flags = f::FLAG_RAW;
    h.orig_len = 3;
    h.crc = 4;
    const Bytes b = headerBytes(h);
    f::Header r = sentinel();
    ASSERT_TRUE(f::readHeader(b.data(), b.size(), r));
    expectSameHeader(r, h);
}

class ReadHeaderRoundTripTest : public ::testing::TestWithParam<int> {};

TEST_P(ReadHeaderRoundTripTest, RandomValidHeadersComeBackIdentical) {
    std::mt19937_64 rng(static_cast<uint64_t>(GetParam()) * 2654435761u + 5);
    for (int t = 0; t < 5; ++t) {
        const f::Header h = randomValidHeader(rng);
        const Bytes b = headerBytes(h);
        ASSERT_EQ(b.size(), f::HEADER_SIZE);
        ASSERT_TRUE(specAccepts(b));
        f::Header r = sentinel();
        ASSERT_TRUE(f::readHeader(b.data(), b.size(), r));
        expectSameHeader(r, h);
        EXPECT_EQ(headerBytes(r), b) << "write(read(bytes)) must give the same bytes";
    }
}

INSTANTIATE_TEST_SUITE_P(Seeds, ReadHeaderRoundTripTest, ::testing::Range(0, 300));

// ===========================================================================
// readHeader: rejections, each leaving the caller's header alone
// ===========================================================================
TEST(ReadHeaderTest, NullDataIsRejectedAtAnySize) {
    for (std::size_t size : {std::size_t{0}, std::size_t{1}, f::HEADER_SIZE, std::size_t{100000}}) {
        f::Header h = sentinel();
        EXPECT_FALSE(f::readHeader(nullptr, size, h)) << size;
        EXPECT_EQ(h.crc, sentinel().crc);
    }
}

TEST(ReadHeaderTest, EveryShortSizeIsRejected) {
    const Bytes full = headerBytes(f::Header{});
    for (std::size_t n = 0; n < f::HEADER_SIZE; ++n) {
        expectRejected(Bytes(full.begin(), full.begin() + n), "size " + std::to_string(n));
    }
}

TEST(ReadHeaderTest, SizeArgumentIsRespectedEvenIfTheBufferIsLonger) {
    const Bytes full = headerBytes(f::Header{});
    f::Header h;
    EXPECT_FALSE(f::readHeader(full.data(), f::HEADER_SIZE - 1, h));
    EXPECT_TRUE(f::readHeader(full.data(), f::HEADER_SIZE, h));
}

TEST(ReadHeaderTest, KraftOverflowIsRejected) {
    f::Header h;
    h.orig_len = 10;
    h.lengths[0] = h.lengths[1] = h.lengths[2] = 1;
    expectRejected(headerBytes(h), "three 1-bit codes");
}

TEST(ReadHeaderTest, KraftOverflowByOneFifteenBitCodeIsRejected) {
    f::Header h;
    h.orig_len = 10;
    for (int s = 0; s < 256; ++s) h.lengths[s] = 8;
    h.lengths[0] = 7;   // 2/256 + 255/256
    expectRejected(headerBytes(h), "one 7 and 255 eights");
}

TEST(ReadHeaderTest, CompleteAndIncompleteCodesAreAccepted) {
    f::Header complete, incomplete;
    complete.orig_len = incomplete.orig_len = 100;
    for (int s = 0; s < 256; ++s) complete.lengths[s] = 8;
    incomplete.lengths[7] = 1;
    incomplete.lengths[8] = 3;
    for (const f::Header& h : {complete, incomplete}) {
        const Bytes b = headerBytes(h);
        f::Header r;
        EXPECT_TRUE(f::readHeader(b.data(), b.size(), r));
        expectSameHeader(r, h);
    }
}

TEST(ReadHeaderTest, HuffmanWithBytesButNoCodesIsRejected) {
    for (uint64_t n : {1ull, 2ull, 1000ull, ~0ull}) {
        f::Header h;
        h.orig_len = n;
        expectRejected(headerBytes(h), "orig_len " + std::to_string(n));
    }
}

TEST(ReadHeaderTest, HuffmanWithCodesButNoBytesIsAccepted) {
    f::Header h;
    h.lengths['x'] = 1;
    const Bytes b = headerBytes(h);
    f::Header r;
    EXPECT_TRUE(f::readHeader(b.data(), b.size(), r));
    expectSameHeader(r, h);
}

TEST(ReadHeaderTest, RawAcceptsAnyLengthWithoutCodes) {
    for (uint64_t n : {0ull, 1ull, 5ull, 1ull << 33, ~0ull}) {
        f::Header h;
        h.flags = f::FLAG_RAW;
        h.orig_len = n;
        h.crc = 99;
        const Bytes b = headerBytes(h);
        f::Header r;
        ASSERT_TRUE(f::readHeader(b.data(), b.size(), r)) << n;
        expectSameHeader(r, h);
    }
}

TEST(ReadHeaderTest, RawWithAnyCodeLengthIsRejected) {
    for (int s = 0; s < 256; ++s) {
        f::Header h;
        h.flags = f::FLAG_RAW;
        h.orig_len = 10;
        h.lengths[s] = static_cast<uint8_t>(1 + s % 15);
        expectRejected(headerBytes(h), "raw with a code for symbol " + std::to_string(s));
    }
}

// Every byte value at every magic position that is not the right one.
class MagicByteTest : public ::testing::TestWithParam<int> {};

TEST_P(MagicByteTest, AnyWrongValueIsRejected) {
    const int pos = GetParam();
    const Bytes good = headerBytes(f::Header{});
    for (int v = 0; v < 256; ++v) {
        if (v == good[pos]) continue;
        Bytes b = good;
        b[pos] = static_cast<uint8_t>(v);
        expectRejected(b, "magic[" + std::to_string(pos) + "] = " + std::to_string(v));
    }
}

INSTANTIATE_TEST_SUITE_P(Positions, MagicByteTest, ::testing::Range(0, 4));

TEST(ReadHeaderTest, MagicIsCaseSensitive) {
    Bytes b = headerBytes(f::Header{});
    b[0] = 'S';
    expectRejected(b, "SWAS");
}

// Every flags byte: 0 and FLAG_RAW are known, any other bit is refused.
class FlagsValueTest : public ::testing::TestWithParam<int> {};

TEST_P(FlagsValueTest, OnlyKnownBitsAreAccepted) {
    const int v = GetParam();
    f::Header h;
    h.flags = static_cast<uint8_t>(v);
    const Bytes b = headerBytes(h);
    if (v == 0 || v == f::FLAG_RAW) {
        f::Header r;
        EXPECT_TRUE(f::readHeader(b.data(), b.size(), r));
        EXPECT_EQ(r.flags, v);
    } else {
        expectRejected(b, "flags " + std::to_string(v));
    }
}

INSTANTIATE_TEST_SUITE_P(EveryByte, FlagsValueTest, ::testing::Range(0, 256));

// A single length of 16 or more, at every symbol.
class OverlongLengthTest : public ::testing::TestWithParam<int> {};

TEST_P(OverlongLengthTest, AnyLengthAboveFifteenIsRejected) {
    const int s = GetParam();
    for (int l : {16, 17, 31, 128, 255}) {
        f::Header h;
        h.orig_len = 1;
        h.lengths[s] = static_cast<uint8_t>(l);
        expectRejected(headerBytes(h), "length " + std::to_string(l) + " at symbol " + std::to_string(s));
    }
    f::Header ok;
    ok.orig_len = 1;
    ok.lengths[s] = 15;
    const Bytes b = headerBytes(ok);
    f::Header r;
    EXPECT_TRUE(f::readHeader(b.data(), b.size(), r));
}

INSTANTIATE_TEST_SUITE_P(EverySymbol, OverlongLengthTest, ::testing::Range(0, 256));

// Random byte mutations of valid headers: readHeader must agree with the
// independent validator on every one, and on success report exactly the bytes.
class ReadHeaderMutationTest : public ::testing::TestWithParam<int> {};

TEST_P(ReadHeaderMutationTest, AgreesWithTheSpecOnMutatedHeaders) {
    std::mt19937_64 rng(static_cast<uint64_t>(GetParam()) * 40503 + 11);
    int accepted = 0;
    for (int t = 0; t < 50; ++t) {
        Bytes b = headerBytes(randomValidHeader(rng));
        const int edits = 1 + static_cast<int>(rng() % 3);
        for (int e = 0; e < edits; ++e) {
            // mostly the interesting fields, sometimes anywhere
            const std::size_t at = rng() % 4 == 0 ? rng() % f::HEADER_SIZE : 4 + rng() % (f::HEADER_SIZE - 4);
            b[at] = rng() % 2 ? static_cast<uint8_t>(rng()) : static_cast<uint8_t>(b[at] ^ (1u << (rng() % 8)));
        }
        const bool want = specAccepts(b);
        f::Header r = sentinel();
        const bool got = f::readHeader(b.data(), b.size(), r);
        ASSERT_EQ(got, want) << "mutation " << t;
        if (got) {
            ++accepted;
            EXPECT_EQ(headerBytes(r), b);
        } else {
            EXPECT_EQ(r.crc, sentinel().crc);
            EXPECT_EQ(r.lengths, sentinel().lengths);
        }
    }
    (void)accepted;
}

INSTANTIATE_TEST_SUITE_P(Seeds, ReadHeaderMutationTest, ::testing::Range(0, 200));

// ===========================================================================
// crc32
// ===========================================================================
struct CrcVector {
    std::string name;
    Bytes data;
    uint32_t crc;
};

std::vector<CrcVector> crcVectors() {
    Bytes ramp(256);
    for (int i = 0; i < 256; ++i) ramp[i] = static_cast<uint8_t>(i);
    // values from Python's zlib.crc32
    return {
        {"empty", {}, 0x00000000u},
        {"a", ts::bytesOf("a"), 0xE8B7BE43u},
        {"abc", ts::bytesOf("abc"), 0x352441C2u},
        {"check", ts::bytesOf("123456789"), 0xCBF43926u},
        {"fox", ts::bytesOf("The quick brown fox jumps over the lazy dog"), 0x414FA339u},
        {"zero", {0x00}, 0xD202EF8Du},
        {"ff", {0xFF}, 0xFF000000u},
        {"zeros32", Bytes(32, 0x00), 0x190A55ADu},
        {"ones32", Bytes(32, 0xFF), 0xFF6CAB0Bu},
        {"ramp", ramp, 0x29058C73u},
        {"swas", ts::bytesOf("swas"), 0xBC9AD4DBu},
        {"abracadabra", ts::bytesOf("abracadabra"), 0x17EAF9B7u},
    };
}

class CrcVectorTest : public ::testing::TestWithParam<CrcVector> {};

TEST_P(CrcVectorTest, MatchesZlib) {
    const CrcVector& v = GetParam();
    EXPECT_EQ(f::crc32(v.data), v.crc);
    EXPECT_EQ(f::crc32(v.data.data(), v.data.size()), v.crc);
    EXPECT_EQ(referenceCrc(v.data), v.crc) << "the reference itself is off";
}

INSTANTIATE_TEST_SUITE_P(Zlib, CrcVectorTest, ::testing::ValuesIn(crcVectors()),
                         [](const auto& info) { return info.param.name; });

TEST(Crc32Test, NullPointerGivesZeroAtAnySize) {
    EXPECT_EQ(f::crc32(nullptr, 0), 0u);
    EXPECT_EQ(f::crc32(nullptr, 1), 0u);
    EXPECT_EQ(f::crc32(nullptr, 1u << 20), 0u);
}

TEST(Crc32Test, EmptyVectorAndZeroSizeGiveZero) {
    const uint8_t data[] = {1, 2, 3};
    EXPECT_EQ(f::crc32(Bytes{}), 0u);
    EXPECT_EQ(f::crc32(data, 0), 0u);
}

TEST(Crc32Test, OnlyTheFirstSizeBytesCount) {
    const Bytes data = ts::bytesOf("123456789xyz");
    EXPECT_EQ(f::crc32(data.data(), 9), 0xCBF43926u);
}

TEST(Crc32Test, LeadingZerosChangeTheCrc) {
    EXPECT_NE(f::crc32(ts::bytesOf(std::string("\0abc", 4))), f::crc32(ts::bytesOf("abc")));
    EXPECT_NE(f::crc32(Bytes{0}), f::crc32(Bytes{}));
    EXPECT_NE(f::crc32(Bytes{0, 0}), f::crc32(Bytes{0}));
    for (int n = 1; n < 64; ++n) EXPECT_NE(f::crc32(Bytes(n, 0)), f::crc32(Bytes(n + 1, 0))) << n;
}

TEST(Crc32Test, VectorOverloadMatchesThePointerOverload) {
    std::mt19937_64 rng(3);
    for (int t = 0; t < 100; ++t) {
        const Bytes data = ts::randomBytes(rng, rng() % 3000);
        EXPECT_EQ(f::crc32(data), f::crc32(data.data(), data.size()));
    }
}

TEST(Crc32Test, MessageFollowedByItsCrcLeavesTheStandardResidue) {
    std::mt19937_64 rng(4);
    for (int t = 0; t < 500; ++t) {
        Bytes data = ts::randomBytes(rng, rng() % 500);
        const uint32_t c = f::crc32(data);
        for (int i = 0; i < 4; ++i) data.push_back(static_cast<uint8_t>(c >> (8 * i)));
        ASSERT_EQ(f::crc32(data), 0x2144DF1Cu) << "trial " << t;
    }
}

TEST(Crc32Test, IsAffineOverXorForEqualLengths) {
    // crc(a ^ b ^ c) == crc(a) ^ crc(b) ^ crc(c) whenever a, b, c have the same length
    std::mt19937_64 rng(5);
    for (int t = 0; t < 500; ++t) {
        const std::size_t n = rng() % 300;
        const Bytes a = ts::randomBytes(rng, n), b = ts::randomBytes(rng, n), c = ts::randomBytes(rng, n);
        Bytes x(n);
        for (std::size_t i = 0; i < n; ++i) x[i] = a[i] ^ b[i] ^ c[i];
        ASSERT_EQ(f::crc32(x), f::crc32(a) ^ f::crc32(b) ^ f::crc32(c)) << "length " << n;
    }
}

TEST(Crc32Test, OneMebibyteMatchesTheReference) {
    std::mt19937_64 rng(6);
    const Bytes data = ts::randomBytes(rng, 1 << 20);
    EXPECT_EQ(f::crc32(data), referenceCrc(data));
}

TEST(Crc32Test, AllTwoBitErrorsInA32ByteMessageAreDetected) {
    std::mt19937_64 rng(7);
    const Bytes msg = ts::randomBytes(rng, 32);
    const uint32_t good = f::crc32(msg);
    int pairs = 0;
    for (int i = 0; i < 256; ++i) {
        for (int j = i + 1; j < 256; ++j) {
            Bytes m = msg;
            m[i / 8] ^= static_cast<uint8_t>(0x80 >> (i % 8));
            m[j / 8] ^= static_cast<uint8_t>(0x80 >> (j % 8));
            ASSERT_NE(f::crc32(m), good) << "bits " << i << "," << j;
            ++pairs;
        }
    }
    EXPECT_EQ(pairs, 256 * 255 / 2);
}

// Every single byte value, alone and followed by every other byte value.
class CrcByteTest : public ::testing::TestWithParam<int> {};

TEST_P(CrcByteTest, OneAndTwoByteMessagesMatchTheReference) {
    const uint8_t b = static_cast<uint8_t>(GetParam());
    EXPECT_EQ(f::crc32(&b, 1), referenceCrc(&b, 1));
    for (int second = 0; second < 256; ++second) {
        const uint8_t m[2] = {b, static_cast<uint8_t>(second)};
        ASSERT_EQ(f::crc32(m, 2), referenceCrc(m, 2)) << second;
    }
}

INSTANTIATE_TEST_SUITE_P(EveryByte, CrcByteTest, ::testing::Range(0, 256));

// Random data of every length 0..4095 (spread over the seeds) against the reference.
class CrcRandomTest : public ::testing::TestWithParam<int> {};

TEST_P(CrcRandomTest, MatchesTheBitwiseReference) {
    std::mt19937_64 rng(static_cast<uint64_t>(GetParam()) + 100);
    for (int n = GetParam(); n < 4096; n += 64) {
        const Bytes data = ts::randomBytes(rng, n);
        ASSERT_EQ(f::crc32(data), referenceCrc(data)) << "length " << n;
    }
}

INSTANTIATE_TEST_SUITE_P(Offsets, CrcRandomTest, ::testing::Range(0, 64));

// Every single-bit error in messages of several lengths changes the CRC.
class CrcSingleBitTest : public ::testing::TestWithParam<int> {};

TEST_P(CrcSingleBitTest, EveryFlippedBitIsDetected) {
    const int n = GetParam();
    std::mt19937_64 rng(n);
    const Bytes msg = ts::randomBytes(rng, n);
    const uint32_t good = f::crc32(msg);
    for (int bit = 0; bit < n * 8; ++bit) {
        Bytes m = msg;
        m[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
        ASSERT_NE(f::crc32(m), good) << "bit " << bit;
    }
}

INSTANTIATE_TEST_SUITE_P(Lengths, CrcSingleBitTest, ::testing::Values(1, 2, 3, 4, 7, 8, 64, 257, 1024));

// Burst errors of every length 1..32 at every position of a 64-byte message.
class CrcBurstTest : public ::testing::TestWithParam<int> {};

TEST_P(CrcBurstTest, EveryBurstUpTo32BitsIsDetected) {
    const int burst = GetParam();
    std::mt19937_64 rng(1000 + burst);
    const Bytes msg = ts::randomBytes(rng, 64);
    const uint32_t good = f::crc32(msg);
    for (int start = 0; start + burst <= 64 * 8; ++start) {
        // a burst of length L flips its first and last bit and anything in between.
        // Bits are numbered low bit first, the order the reflected CRC consumes
        // them in: only there is a burst contiguous in the polynomial.
        uint64_t pattern = (rng() | 1ull) & ts::maskTo(~0ull, burst);
        pattern |= 1ull << (burst - 1);
        Bytes m = msg;
        for (int k = 0; k < burst; ++k) {
            if ((pattern >> k) & 1) {
                const int bit = start + k;
                m[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
            }
        }
        ASSERT_NE(f::crc32(m), good) << "burst " << burst << " at " << start;
    }
}

INSTANTIATE_TEST_SUITE_P(OneTo32, CrcBurstTest, ::testing::Range(1, 33));
