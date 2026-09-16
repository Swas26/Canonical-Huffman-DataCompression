// test_bitio.cpp -- BitWriter -> BitReader round trip, no framework needed.
// Not a single line of Huffman code is involved: if this passes, bit I/O is off
// the suspect list for good.
#include "BitReader.hpp"
#include "BitWriter.hpp"

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;

static int passed = 0, failed = 0;

static void check(const std::string& name, bool cond, const std::string& detail = "") {
    if (cond) {
        ++passed;
        std::cout << "[PASS] " << name << '\n';
    } else {
        ++failed;
        std::cout << "[FAIL] " << name << '\n';
        if (!detail.empty()) std::cout << "       " << detail << '\n';
    }
}

static std::string hex(uint64_t v) {
    std::ostringstream os;
    os << "0x" << std::hex << std::uppercase << v;
    return os.str();
}

// Low `width` bits of `v`. width is 1..64, so the 1ull << 64 trap is avoided.
static uint64_t mask_to(uint64_t v, int width) {
    return width >= 64 ? v : v & ((1ull << width) - 1);
}

struct Pair {
    uint64_t raw;      // what gets handed to write_bits, high junk included
    uint64_t value;    // what must come back out
    int width;
};

// ---------------------------------------------------------------------------
// The main event: 50,000 random (value, width) pairs, fixed seed.
// ---------------------------------------------------------------------------
static void round_trip_50k() {
    const int N = 50000;
    std::mt19937_64 rng(20260916);   // fixed seed -> byte-identical every run

    std::vector<Pair> pairs;
    pairs.reserve(N);
    uint64_t total_bits = 0;
    for (int i = 0; i < N; ++i) {
        int width = static_cast<int>(rng() % 64) + 1;   // 1..64
        uint64_t raw = rng();                           // junk above `width` too
        pairs.push_back({raw, mask_to(raw, width), width});
        total_bits += static_cast<uint64_t>(width);
    }

    Bytes buf;
    uint64_t bits_written = 0;
    {
        BitWriter w(buf);
        for (const Pair& p : pairs) w.write_bits(p.raw, p.width);
        bits_written = w.bits_written();
        w.flush();
    }

    check("writer counted every bit it was handed",
          bits_written == total_bits,
          "want " + std::to_string(total_bits) + ", got " + std::to_string(bits_written));
    check("buffer is exactly ceil(bits/8) bytes",
          buf.size() == (total_bits + 7) / 8,
          "want " + std::to_string((total_bits + 7) / 8) + " bytes, got " +
              std::to_string(buf.size()));

    // Read everything back. Each symbol is checked three ways: peeked at its own
    // width, peeked through a fixed 64-bit window (how the session-6 table
    // decoder will look at the stream), then actually read.
    BitReader r(buf);
    int read_bad = 0, peek_bad = 0, window_bad = 0, advance_bad = 0;
    std::string first_failure;

    for (int i = 0; i < N; ++i) {
        const Pair& p = pairs[i];

        uint64_t peeked = r.peek_bits(p.width);
        uint64_t before = r.bits_read();
        uint64_t window = r.peek_bits(64) >> (64 - p.width);
        if (r.bits_read() != before) ++advance_bad;

        uint64_t got = r.read_bits(p.width);

        if (peeked != p.value) ++peek_bad;
        if (window != p.value) ++window_bad;
        if (got != p.value) {
            ++read_bad;
            if (first_failure.empty()) {
                first_failure = "symbol #" + std::to_string(i) + " width " +
                                std::to_string(p.width) + ": want " + hex(p.value) +
                                ", got " + hex(got);
            }
        }
    }

    check("50,000 random (value, width) pairs survive the round trip",
          read_bad == 0,
          std::to_string(read_bad) + " mismatches; first: " + first_failure);
    check("peek_bits(width) agrees with read_bits(width) at all 50,000 positions",
          peek_bad == 0, std::to_string(peek_bad) + " mismatches");
    check("peek_bits(64) window holds the next code in its top bits",
          window_bad == 0, std::to_string(window_bad) + " mismatches");
    check("peek_bits never advances the reader",
          advance_bad == 0, std::to_string(advance_bad) + " advances");
    check("reader consumed exactly as many bits as the writer produced",
          r.bits_read() == total_bits,
          "want " + std::to_string(total_bits) + ", got " + std::to_string(r.bits_read()));
    check("no overrun: the trailing pad bits were never touched", r.ok());

    // The framing decision in practice: the header would carry N, the decoder
    // stops after N symbols, and the leftover pad bits in the last byte are
    // simply whatever they are. Prove they are reachable but irrelevant.
    check("pad bits remain in the buffer, unread",
          r.bits_remaining() == buf.size() * 8 - total_bits,
          "remaining = " + std::to_string(r.bits_remaining()));
}

// ---------------------------------------------------------------------------
// Edge cases around the seams.
// ---------------------------------------------------------------------------
static void edge_cases() {
    {
        Bytes buf;
        BitReader r(buf);
        check("empty stream is exhausted from the start", r.exhausted() && r.ok());
        check("peek on an empty stream returns 0 and stays clean",
              r.peek_bits(64) == 0 && r.ok());
        r.read_bits(1);
        check("reading an empty stream sets the overrun flag", !r.ok());
    }
    {
        Bytes buf;
        { BitWriter w(buf); w.write_bits(0b101, 3); }
        BitReader r(buf);
        check("3 bits are left-aligned in the byte", buf == Bytes{0xA0});
        check("reading them back gives 0b101", r.read_bits(3) == 0b101);
        check("the 5 pad bits are still there", r.bits_remaining() == 5);
        check("pad bits read as zero", r.read_bits(5) == 0 && r.ok());
    }
    {
        Bytes buf;
        { BitWriter w(buf); w.write_bits(0xDEADBEEFCAFEBABEull, 64); }
        BitReader r(buf);
        check("a full 64-bit value round-trips", r.read_bits(64) == 0xDEADBEEFCAFEBABEull);
    }
    {
        Bytes buf;
        { BitWriter w(buf); w.write_bits(0xFF, 0); w.write_bit(true); }
        BitReader r(buf);
        check("width 0 writes nothing", buf == Bytes{0x80});
        check("read_bits(0) returns 0 and consumes nothing",
              r.read_bits(0) == 0 && r.bits_read() == 0);
        check("the next bit is still the one that was written", r.read_bit());
    }
    {
        Bytes buf;
        { BitWriter w(buf); w.write_bits(0xAB, 8); w.write_bits(0xCD, 8); }
        BitReader r(buf);
        r.skip_bits(8);
        check("skip_bits jumps a whole symbol", r.read_bits(8) == 0xCD);
    }
    {
        Bytes buf;
        {
            BitWriter w(buf);
            w.write_bits(0b111, 3);
            w.flush();                  // writer pads to the byte boundary
            w.write_bits(0xAA, 8);
        }
        BitReader r(buf);
        check("read side mirrors a mid-stream flush", r.read_bits(3) == 0b111);
        r.align_to_byte();
        check("align_to_byte lands on the next byte", r.read_bits(8) == 0xAA);
        check("align_to_byte is a no-op when already aligned",
              (r.align_to_byte(), r.bits_read() == 16));
    }
    {
        // Overrun: ask for more than the stream holds.
        Bytes buf;
        { BitWriter w(buf); w.write_bits(0b1011, 4); }   // 4 real bits + 4 pad
        BitReader r(buf);
        uint64_t got = r.read_bits(16);
        check("an over-long read zero-fills the missing bits",
              got == (0b10110000ull << 8));
        check("an over-long read sets the overrun flag", !r.ok());
    }
    {
        // peek must NOT flag overrun -- the table decoder depends on this.
        Bytes buf;
        { BitWriter w(buf); w.write_bits(0b1, 1); }
        BitReader r(buf);
        check("peek past the end zero-fills without flagging overrun",
              r.peek_bits(64) == (0b10000000ull << 56) && r.ok());
        check("peek is repeatable", r.peek_bits(64) == r.peek_bits(64) && r.ok());
    }
}

int main() {
    round_trip_50k();
    edge_cases();

    std::cout << '\n' << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
