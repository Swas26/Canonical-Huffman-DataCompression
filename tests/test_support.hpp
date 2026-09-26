// test_support.hpp -- helpers shared by the test files: byte buffers, seeded
// data generators in the shapes the codec meets, and a scratch directory.
// Header-only; a test file includes it and uses what it needs.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace ts {

using Bytes = std::vector<uint8_t>;
namespace fs = std::filesystem;

inline Bytes bytesOf(const std::string& s) { return Bytes(s.begin(), s.end()); }

inline Bytes randomBytes(std::mt19937_64& rng, std::size_t n) {
    Bytes b(n);
    for (auto& x : b) x = static_cast<uint8_t>(rng());
    return b;
}

// Low `width` bits of `v`, width 0..64 (1ull << 64 is undefined, so 64 is special).
inline uint64_t maskTo(uint64_t v, int width) {
    if (width <= 0) return 0;
    return width >= 64 ? v : v & ((1ull << width) - 1);
}

// ---------------------------------------------------------------------------
// Input shapes. Each one exercises a different part of the codec: a single
// symbol (1-bit codes, incomplete code), flat alphabets, skewed ones that
// reach past the 9-bit fast table, Fibonacci counts that force the 15-bit
// limiter, and random bytes that must fall back to raw storage.
// ---------------------------------------------------------------------------
enum class Shape {
    OneSymbol,      // a single byte value repeated
    TwoSymbols,     // two values, random order
    SmallAlphabet,  // 5..16 values, flat
    Geometric,      // byte k with probability ~2^-(k+1): short and long codes
    Text,           // English-like words, spaces, punctuation, newlines
    Runs,           // random bytes in runs of 1..64
    Ramp,           // 0, 1, 2, ..., 255, 0, 1, ...
    Fibonacci,      // counts 1, 1, 2, 3, 5, ... shuffled: deepest possible trees
    Uniform,        // random bytes: nothing to gain, stored raw
};

inline const char* shapeName(Shape s) {
    switch (s) {
        case Shape::OneSymbol:     return "OneSymbol";
        case Shape::TwoSymbols:    return "TwoSymbols";
        case Shape::SmallAlphabet: return "SmallAlphabet";
        case Shape::Geometric:     return "Geometric";
        case Shape::Text:          return "Text";
        case Shape::Runs:          return "Runs";
        case Shape::Ramp:          return "Ramp";
        case Shape::Fibonacci:     return "Fibonacci";
        case Shape::Uniform:       return "Uniform";
    }
    return "Unknown";
}

inline const std::vector<Shape>& allShapes() {
    static const std::vector<Shape> shapes = {
        Shape::OneSymbol, Shape::TwoSymbols, Shape::SmallAlphabet, Shape::Geometric, Shape::Text,
        Shape::Runs,      Shape::Ramp,       Shape::Fibonacci,     Shape::Uniform,
    };
    return shapes;
}

inline Bytes makeData(Shape shape, std::size_t n, uint64_t seed) {
    std::mt19937_64 rng(seed * 0x9E3779B97F4A7C15ull + static_cast<uint64_t>(shape) + 1);
    Bytes b;
    b.reserve(n);

    switch (shape) {
        case Shape::OneSymbol: {
            b.assign(n, static_cast<uint8_t>(rng()));
            break;
        }
        case Shape::TwoSymbols: {
            const uint8_t x = static_cast<uint8_t>(rng());
            const uint8_t y = static_cast<uint8_t>(x + 1 + rng() % 255);
            for (std::size_t i = 0; i < n; ++i) b.push_back(rng() & 1 ? x : y);
            break;
        }
        case Shape::SmallAlphabet: {
            const int k = 5 + static_cast<int>(rng() % 12);
            std::vector<uint8_t> alphabet(256);
            for (int i = 0; i < 256; ++i) alphabet[i] = static_cast<uint8_t>(i);
            std::shuffle(alphabet.begin(), alphabet.end(), rng);
            for (std::size_t i = 0; i < n; ++i) b.push_back(alphabet[rng() % k]);
            break;
        }
        case Shape::Geometric: {
            const uint8_t base = static_cast<uint8_t>(rng());
            for (std::size_t i = 0; i < n; ++i) {
                int k = 0;
                while (k < 40 && (rng() & 1)) ++k;
                b.push_back(static_cast<uint8_t>(base + k));
            }
            break;
        }
        case Shape::Text: {
            static const char* const WORDS[] = {
                "the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog", "huffman",
                "canonical", "code", "length", "table", "bit", "byte", "stream", "a", "of",
                "and", "to", "in", "is", "compression", "entropy", "decoder", "encoder",
            };
            static const char* const PUNCT[] = {" ", " ", " ", " ", ", ", ". ", "\n", "; "};
            while (b.size() < n) {
                const std::string w = WORDS[rng() % (sizeof WORDS / sizeof *WORDS)];
                const std::string p = PUNCT[rng() % (sizeof PUNCT / sizeof *PUNCT)];
                for (char c : w + p) b.push_back(static_cast<uint8_t>(c));
            }
            b.resize(n);
            break;
        }
        case Shape::Runs: {
            while (b.size() < n) {
                const uint8_t v = static_cast<uint8_t>(rng());
                const std::size_t len = 1 + rng() % 64;
                for (std::size_t i = 0; i < len && b.size() < n; ++i) b.push_back(v);
            }
            break;
        }
        case Shape::Ramp: {
            for (std::size_t i = 0; i < n; ++i) b.push_back(static_cast<uint8_t>(i));
            break;
        }
        case Shape::Fibonacci: {
            // symbol k appears F(k+1) times while the total fits, the rest goes to the
            // most common one. Same data every time for a given n: only the order is seeded.
            std::vector<uint8_t> symbols(256);
            for (int i = 0; i < 256; ++i) symbols[i] = static_cast<uint8_t>(i);
            std::shuffle(symbols.begin(), symbols.end(), rng);
            uint64_t a = 1, c = 1;
            int k = 0;
            while (k < 90 && b.size() + a <= n) {
                b.insert(b.end(), a, symbols[k]);
                const uint64_t next = a + c;
                a = c;
                c = next;
                ++k;
            }
            if (b.size() < n) b.insert(b.end(), n - b.size(), k > 0 ? symbols[k - 1] : symbols[0]);
            std::shuffle(b.begin(), b.end(), rng);
            break;
        }
        case Shape::Uniform: {
            b = randomBytes(rng, n);
            break;
        }
    }
    return b;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
inline Bytes readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline void writeFile(const fs::path& p, const Bytes& data) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

inline std::string readText(const fs::path& p) {
    const Bytes b = readFile(p);
    return std::string(b.begin(), b.end());
}

// A fresh directory under the system temp dir, removed with everything in it
// when the object goes away.
class TempDir {
public:
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "scomp-test-XXXXXX").string();
        if (::mkdtemp(tmpl.data()) != nullptr) dir = tmpl;
    }
    ~TempDir() {
        std::error_code ec;
        if (!dir.empty()) fs::remove_all(dir, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    bool ok() const { return !dir.empty(); }
    const fs::path& path() const { return dir; }
    std::string operator/(const std::string& name) const { return (dir / name).string(); }

    // Names in the directory, sorted: lets a test prove no temp file was left behind.
    std::vector<std::string> list() const {
        std::vector<std::string> names;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) names.push_back(e.path().filename().string());
        std::sort(names.begin(), names.end());
        return names;
    }

private:
    fs::path dir;
};

}  // namespace ts
