#include "cli/Cli.hpp"

#include "core/Codec.hpp"
#include "core/Format.hpp"
#include "core/Huffman.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
using cli::Command;
using cli::Options;

namespace {
    const char* const PROG = "swas";

    const char* const USAGE =
        "usage: swas <command> [options] [file...]\n"
        "\n"
        "commands\n"
        "  compress, c      file -> file.swas\n"
        "  decompress, d    file.swas -> file\n"
        "  test, t          decode archives and check their crc, write nothing\n"
        "  info, i          an archive's header and code lengths\n"
        "  bench, b         time encode and both decoders on files, check the round trip\n"
        "  help             this text\n"
        "\n"
        "options                                                             for\n"
        "  -o FILE      output name, one input only; - is stdout             c d\n"
        "  -c           write to stdout                                      c d\n"
        "  -f           overwrite outputs, compress .swas files again,       c d\n"
        "               allow archives to or from a terminal\n"
        "  --rm         delete each input once its output is written         c d\n"
        "  -q           say nothing on success                               c d t\n"
        "  --bitbybit   decode with the bit by bit decoder                   d t\n"
        "  --codes      also list every symbol's canonical code              i\n"
        "  -n RUNS      timed runs per step, default as many as fit in 0.2s  b\n"
        "\n"
        "With no file, or -, reads stdin and writes stdout. Inputs are kept unless --rm.\n"
        "Exit status: 0 ok, 1 a file failed, 2 bad usage.\n";

    bool endsWith(const std::string& s, const char* suffix){
        const std::size_t n = std::strlen(suffix);
        return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
    }

    bool parseRuns(const std::string& s, int& runs){
        if (s.empty()) return false;
        char* end = nullptr;
        errno = 0;
        const long v = std::strtol(s.c_str(), &end, 10);
        if (errno != 0 || *end != '\0' || v < 1 || v > 1000000) return false;
        runs = static_cast<int>(v);
        return true;
    }
}

bool cli::parseArgs(const std::vector<std::string>& args, Options& o, std::string& err){
    o = Options{};
    if (args.empty()){
        err = "missing command";
        return false;
    }

    const std::string& name = args[0];
    if (name == "compress" || name == "c") o.cmd = Command::compress;
    else if (name == "decompress" || name == "d" || name == "x") o.cmd = Command::decompress;
    else if (name == "test" || name == "t") o.cmd = Command::test;
    else if (name == "info" || name == "i") o.cmd = Command::info;
    else if (name == "bench" || name == "b") o.cmd = Command::bench;
    else if (name == "help" || name == "-h" || name == "--help"){
        o.cmd = Command::help;
        return true;
    } else {
        err = "unknown command '" + name + "'";
        return false;
    }

    const bool writes = o.cmd == Command::compress || o.cmd == Command::decompress;
    const bool decodes = o.cmd == Command::decompress || o.cmd == Command::test;

    /* a flag the command does not use is an error, not something to ignore: "swas t -o x" would
        otherwise look like it wrote x */
    auto takes = [&](bool allowed, const std::string& flag){
        if (!allowed) err = flag + " does not apply to " + name;
        return allowed;
    };

    bool endOfOptions = false;
    for (std::size_t i = 1; i < args.size(); ++i){
        const std::string& a = args[i];
        if (endOfOptions || a == "-" || a.empty() || a[0] != '-'){
            o.files.push_back(a);
            continue;
        }
        if (a == "--"){
            endOfOptions = true;
            continue;
        }
        if (a == "-h" || a == "--help"){
            o.cmd = Command::help;
            return true;
        }

        if (a[1] == '-'){
            if (a == "--rm"){
                if (!takes(writes, a)) return false;
                o.remove = true;
            } else if (a == "--bitbybit"){
                if (!takes(decodes, a)) return false;
                o.bitbyByte = true;
            } else if (a == "--codes"){
                if (!takes(o.cmd == Command::info, a)) return false;
                o.codes = true;
            } else {
                err = "unknown option " + a;
                return false;
            }
            continue;
        }

        /* short flags bundle, -fq. -o and -n take the rest of the bundle as their value, or the next argument */
        for (std::size_t j = 1; j < a.size(); ++j){
            const char f = a[j];
            const std::string flag = std::string("-") + f;

            if (f == 'o' || f == 'n'){
                std::string value;
                if (j + 1 < a.size()) value = a.substr(j + 1);
                else if (i + 1 < args.size()) value = args[++i];
                else {
                    err = flag + " needs a value";
                    return false;
                }

                if (f == 'o'){
                    if (!takes(writes, flag)) return false;
                    o.output = value;
                } else {
                    if (!takes(o.cmd == Command::bench, flag)) return false;
                    if (!parseRuns(value, o.runs)){
                        err = "-n wants a whole number of runs from 1 to 1000000, not '" + value + "'";
                        return false;
                    }
                }
                break;
            }

            if (f == 'c'){
                if (!takes(writes, flag)) return false;
                o.toStdout = true;
            } else if (f == 'f'){
                if (!takes(writes, flag)) return false;
                o.force = true;
            } else if (f == 'q'){
                if (!takes(writes || o.cmd == Command::test, flag)) return false;
                o.quiet = true;
            } else {
                err = "unknown option " + flag;
                return false;
            }
        }
    }

    if (o.files.empty()) o.files.push_back("-");

    if (std::count(o.files.begin(), o.files.end(), "-") > 1){
        err = "stdin (-) can only be read once";
        return false;
    }
    if (!o.output.empty() && o.toStdout){
        err = "-o and -c both say where the output goes, give one";
        return false;
    }
    if (!o.output.empty() && o.files.size() > 1){
        err = "-o takes one input file, got " + std::to_string(o.files.size());
        return false;
    }
    /* an archive holds one file, so archives back to back on stdout would not decompress. decompress -c
        is fine, the originals just come out one after another */
    if (o.cmd == Command::compress && o.toStdout && o.files.size() > 1){
        err = "-c compresses one file at a time, got " + std::to_string(o.files.size());
        return false;
    }
    /* the only copy left would be in a pipe that may never get read */
    if (o.remove && (o.toStdout || o.output == "-")){
        err = "--rm needs an output file, not stdout";
        return false;
    }
    return true;
}

bool cli::outputPath(const std::string& in, Command cmd, std::string& out){
    if (cmd == Command::compress){
        out = in + SUFFIX;
        return true;
    }
    if (cmd != Command::decompress || !endsWith(in, SUFFIX)) return false;

    /* the part in front of the suffix has to be a file name: "" and "dir/" are not */
    const std::string stem = in.substr(0, in.size() - std::strlen(SUFFIX));
    if (stem.empty() || stem.back() == '/') return false;
    out = stem;
    return true;
}

std::string cli::humanSize(uint64_t bytes){
    char buf[32];
    if (bytes < 1000){
        std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
        return buf;
    }

    /* decimal units, like Finder and the MB/s in bench: 1 MB = 1,000,000 bytes */
    static const char* const UNITS[] = {"KB", "MB", "GB", "TB"};
    double v = bytes / 1000.0;
    int u = 0;
    /* 999.95 not 1000: anything that would print as "1000.0" goes up a unit instead */
    while (v >= 999.95 && u < 3){
        v /= 1000.0;
        ++u;
    }
    std::snprintf(buf, sizeof buf, "%.1f %s", v, UNITS[u]);
    return buf;
}

namespace {
    using Clock = std::chrono::steady_clock;

    double secondsSince(Clock::time_point t0){
        return std::chrono::duration<double>(Clock::now() - t0).count();
    }

    /* what a path is called in messages */
    std::string shown(const std::string& path, bool output = false){
        if (path != "-") return path;
        return output ? "(stdout)" : "(stdin)";
    }

    /* archive size as a share of the original, the one ratio every report uses */
    std::string percent(uint64_t archive, uint64_t original){
        if (original == 0) return "--";
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.1f%%", 100.0 * static_cast<double>(archive) / static_cast<double>(original));
        return buf;
    }

    void fail(const std::string& what, const std::string& why){
        std::fprintf(stderr, "%s: %s: %s\n", PROG, what.c_str(), why.c_str());
    }

    /* the whole of path, or of stdin for "-". on false, err says why */
    bool readInput(const std::string& path, Bytes& data, std::string& err){
        data.clear();
        FILE* f = stdin;
        if (path != "-"){
            std::error_code ec;
            if (fs::is_directory(path, ec)){
                err = "is a directory";
                return false;
            }
            f = std::fopen(path.c_str(), "rb");
            if (f == nullptr){
                err = std::strerror(errno);
                return false;
            }
            const auto size = fs::file_size(path, ec);
            if (!ec) data.reserve(static_cast<std::size_t>(size));
        }

        uint8_t buf[1 << 16];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.insert(data.end(), buf, buf + n);

        const bool failed = std::ferror(f) != 0;
        const int e = errno;
        if (f != stdin) std::fclose(f);
        if (failed){
            err = std::strerror(e);
            return false;
        }
        return true;
    }

    /* the mode a new output gets: the input's permission bits, or what umask allows for stdin */
    mode_t modeFor(const std::string& in){
        struct stat st;
        if (in != "-" && ::stat(in.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return st.st_mode & 0777;
        const mode_t mask = ::umask(0);
        ::umask(mask);
        return 0666 & ~mask;
    }

    /* out gets all of data or nothing: the bytes go to a temp file next to out, which is renamed over
        it only once every one of them was written. so a full disk or a kill mid write never leaves
        half a file under the real name. "-" is stdout */
    bool writeOutput(const std::string& out, const Bytes& data, mode_t mode, std::string& err){
        if (out == "-"){
            if ((!data.empty() && std::fwrite(data.data(), 1, data.size(), stdout) != data.size())
                || std::fflush(stdout) != 0){
                err = std::strerror(errno);
                return false;
            }
            return true;
        }

        std::string tmp = out + ".XXXXXX";
        const int fd = ::mkstemp(tmp.data());
        if (fd < 0){
            err = std::strerror(errno);
            return false;
        }
        FILE* f = ::fdopen(fd, "wb");
        if (f == nullptr){
            err = std::strerror(errno);
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }

        bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
        int e = errno;
        /* fclose flushes, so a write error can first show up here */
        if (std::fclose(f) != 0 && ok){
            ok = false;
            e = errno;
        }
        if (ok && ::chmod(tmp.c_str(), mode) != 0){
            ok = false;
            e = errno;
        }
        if (ok && std::rename(tmp.c_str(), out.c_str()) != 0){
            ok = false;
            e = errno;
        }
        if (!ok){
            ::unlink(tmp.c_str());
            err = std::strerror(e);
        }
        return ok;
    }

    cd::Decoder decoderFor(const Options& o){
        return o.bitbyByte ? cd::Decoder::BitbyBit : cd::Decoder::Table;
    }

    bool storedRaw(const Bytes& archive){
        f::Header h;
        return f::readHeader(archive.data(), archive.size(), h) && (h.flags & f::FLAG_RAW);
    }

    struct Totals {
        int files = 0;
        uint64_t archive = 0;
        uint64_t original = 0;
    };

    /* compress or decompress one file. every check that can refuse runs before the input is read,
        and nothing is written or removed until the result is known good */
    bool convertOne(const Options& o, const std::string& in, Totals& totals){
        const bool compress = o.cmd == Command::compress;
        const bool fromStdin = in == "-";

        std::string out;
        if (o.toStdout || (fromStdin && o.output.empty())) out = "-";
        else if (!o.output.empty()) out = o.output;
        else if (!cli::outputPath(in, o.cmd, out)){
            fail(in, std::string("does not end in ") + cli::SUFFIX + ", give the output name with -o, or use -c");
            return false;
        }

        if (compress && !fromStdin && !o.force && endsWith(in, cli::SUFFIX)){
            fail(in, std::string("already ends in ") + cli::SUFFIX + ", skipped (-f compresses it anyway)");
            return false;
        }

        if (out != "-"){
            std::error_code ec;
            if (!fromStdin && fs::equivalent(in, out, ec)){
                fail(in, "input and output are the same file");
                return false;
            }
            /* symlink_status so a dangling link counts as there too */
            if (!o.force && fs::exists(fs::symlink_status(out, ec))){
                fail(out, "already exists (-f overwrites it)");
                return false;
            }
        }

        const auto t0 = Clock::now();
        std::string err;
        Bytes data;
        if (!readInput(in, data, err)){
            fail(shown(in), "cannot read: " + err);
            return false;
        }

        Bytes result;
        const cd::Status s = compress ? cd::encode(data, result) : cd::decode(data, result, decoderFor(o));
        if (s != cd::Status::Ok){
            fail(shown(in), cd::messge(s));
            return false;
        }

        /* --rm is about to delete the only other copy, so the archive has to prove it decodes back
            first. decompress needs no such step, decode already checked the crc */
        if (compress && o.remove && !fromStdin){
            Bytes back;
            if (cd::decode(result, back) != cd::Status::Ok || back != data){
                fail(in, "the archive did not decode back to the input, input kept");
                return false;
            }
        }

        if (!writeOutput(out, result, modeFor(in), err)){
            fail(shown(out, true), "cannot write: " + err);
            return false;
        }

        if (o.remove && !fromStdin){
            std::error_code ec;
            fs::remove(in, ec);
            if (ec){
                fail(in, "output written, but the input could not be removed: " + ec.message());
                return false;
            }
        }

        const uint64_t archive = compress ? result.size() : data.size();
        const uint64_t original = compress ? data.size() : result.size();
        ++totals.files;
        totals.archive += archive;
        totals.original += original;

        if (!o.quiet){
            std::fprintf(stderr, "%s -> %s  %s -> %s  %s  %.1f ms%s\n",
                         shown(in).c_str(), shown(out, true).c_str(),
                         cli::humanSize(data.size()).c_str(), cli::humanSize(result.size()).c_str(),
                         percent(archive, original).c_str(), secondsSince(t0) * 1e3,
                         compress && storedRaw(result) ? "  (stored raw)" : "");
        }
        return true;
    }

    bool testOne(const Options& o, const std::string& in){
        std::string err;
        Bytes data, out;
        if (!readInput(in, data, err)){
            fail(shown(in), "cannot read: " + err);
            return false;
        }
        const cd::Status s = cd::decode(data, out, decoderFor(o));
        if (s != cd::Status::Ok){
            fail(shown(in), cd::messge(s));
            return false;
        }
        if (!o.quiet) std::printf("%s: ok, %s\n", shown(in).c_str(), cli::humanSize(out.size()).c_str());
        return true;
    }

    /* 'e' for printable bytes, 0x0a for the rest */
    std::string symbolName(int s){
        char buf[8];
        if (s >= 0x20 && s <= 0x7e) std::snprintf(buf, sizeof buf, "'%c'", s);
        else std::snprintf(buf, sizeof buf, "0x%02x", s);
        return buf;
    }

    /* the low len bits of code, most significant first, the order BitWriter puts them in the stream */
    std::string bitString(uint32_t code, int len){
        std::string bits;
        for (int i = len - 1; i >= 0; --i) bits += ((code >> i) & 1) ? '1' : '0';
        return bits;
    }

    /* reads only the header, so it is instant on any size but vouches for nothing past it. test does that */
    bool infoOne(const Options& o, const std::string& in, bool first){
        std::string err;
        Bytes data;
        if (!readInput(in, data, err)){
            fail(shown(in), "cannot read: " + err);
            return false;
        }
        f::Header h;
        if (!f::readHeader(data.data(), data.size(), h)){
            fail(shown(in), cd::messge(cd::Status::BadHeader));
            return false;
        }

        if (!first) std::printf("\n");
        const bool raw = h.flags & f::FLAG_RAW;
        const uint64_t payload = data.size() - f::HEADER_SIZE;

        std::printf("%s\n", shown(in).c_str());
        std::printf("  format    %s\n", raw ? "raw, stored as is (huffman would not have been smaller)" : "huffman");
        std::printf("  original  %s (%llu bytes)\n", cli::humanSize(h.orig_len).c_str(),
                    static_cast<unsigned long long>(h.orig_len));
        std::printf("  archive   %s (%zu bytes, %zu of them header), %s of the original\n",
                    cli::humanSize(data.size()).c_str(), data.size(), f::HEADER_SIZE,
                    percent(data.size(), h.orig_len).c_str());
        std::printf("  crc32     %08x\n", h.crc);
        if (raw) return true;

        uint32_t count[hf::MAX_CODE_LEN + 1] = {};
        int used = 0;
        for (int s = 0; s < 256; ++s){
            if (h.lengths[s] == 0) continue;
            ++count[h.lengths[s]];
            ++used;
        }
        if (used == 0) return true; /* a huffman header for an empty file, valid but nothing to show */

        const int maxLen = hf::maxLength(h.lengths);
        int minLen = 1;
        while (count[minLen] == 0) ++minLen;

        std::printf("  symbols   %d of 256, codes %d to %d bits\n", used, minLen, maxLen);
        if (h.orig_len > 0){
            std::printf("  payload   %.2f bits per byte, padding included\n",
                        static_cast<double>(payload) * 8.0 / static_cast<double>(h.orig_len));
        }

        /* how many symbols got each length. rows in between with no symbols stay in, the gaps are the shape */
        const uint32_t most = *std::max_element(count + 1, count + hf::MAX_CODE_LEN + 1);
        std::printf("\n  bits  symbols\n");
        for (int l = minLen; l <= maxLen; ++l){
            const int bar = count[l] == 0 ? 0 : std::max(1, static_cast<int>(count[l] * 40 / most));
            std::printf("  %4d  %7u%s%s\n", l, count[l], bar ? "  " : "", std::string(bar, '#').c_str());
        }

        if (o.codes){
            hf::CodeTable codes{};
            hf::buildCanonicalCodes(h.lengths, codes); /* readHeader already checked the lengths */

            /* in the order the codes were handed out: by length, then by symbol */
            std::vector<int> order;
            for (int s = 0; s < 256; ++s){
                if (h.lengths[s] != 0) order.push_back(s);
            }
            std::stable_sort(order.begin(), order.end(),
                             [&](int a, int b){ return h.lengths[a] < h.lengths[b]; });

            std::printf("\n  symbol  bits  code\n");
            for (int s : order){
                std::printf("  %-6s  %4d  %s\n", symbolName(s).c_str(), h.lengths[s],
                            bitString(codes[s], h.lengths[s]).c_str());
            }
        }
        return true;
    }

    /* the best of the timed runs, in seconds: runs of them when given, else as many as fit in about
        0.2 s and at least one. best, not mean, because anything the machine does meanwhile only adds */
    template <class F>
    double timeBest(int runs, F&& step){
        double best = 0, spent = 0;
        for (int i = 0; runs > 0 ? i < runs : (i == 0 || spent < 0.2); ++i){
            const auto t0 = Clock::now();
            step();
            const double t = secondsSince(t0);
            best = i == 0 ? t : std::min(best, t);
            spent += t;
        }
        return std::max(best, 1e-9); /* a rate never divides by zero */
    }

    bool benchOne(const Options& o, const std::string& in, bool first){
        std::string err;
        Bytes data;
        if (!readInput(in, data, err)){
            fail(shown(in), "cannot read: " + err);
            return false;
        }

        if (!first) std::printf("\n");
        if (data.empty()){
            std::printf("%s  empty, nothing to measure\n", shown(in).c_str());
            return true;
        }

        Bytes archive, table, bitwise;
        cd::Status tableStatus = cd::Status::Ok, bitStatus = cd::Status::Ok;
        const double encode = timeBest(o.runs, [&]{ cd::encode(data, archive); });
        const double decTable = timeBest(o.runs, [&]{ tableStatus = cd::decode(archive, table, cd::Decoder::Table); });
        const double decBit = timeBest(o.runs, [&]{ bitStatus = cd::decode(archive, bitwise, cd::Decoder::BitbyBit); });

        f::Header h;
        f::readHeader(archive.data(), archive.size(), h); /* our own encode's output */
        const bool raw = h.flags & f::FLAG_RAW;

        /* shannon entropy of the byte counts: no code that spends a whole number of bits per byte, huffman
            included, can average below it */
        const auto freq = hf::countFrequencies(data);
        const double n = static_cast<double>(data.size());
        double entropy = 0;
        uint64_t bits = 0;
        for (int s = 0; s < 256; ++s){
            if (freq[s] == 0) continue;
            const double p = static_cast<double>(freq[s]) / n;
            entropy -= p * std::log2(p);
            bits += freq[s] * h.lengths[s];
        }

        auto rate = [&](double seconds){ return n / seconds / 1e6; };

        std::printf("%s  %s -> %s  %s\n", shown(in).c_str(), cli::humanSize(data.size()).c_str(),
                    cli::humanSize(archive.size()).c_str(), percent(archive.size(), data.size()).c_str());
        std::printf("  %-18s  %9.3f bits/byte\n", "entropy", entropy);
        if (raw) std::printf("  %-18s  stored raw, codes would not have beaten the input\n", "huffman");
        else std::printf("  %-18s  %9.3f bits/byte, %.3f over the entropy\n", "huffman", bits / n, bits / n - entropy);
        std::printf("  %-18s  %9.3f ms  %8.1f MB/s\n", "encode", encode * 1e3, rate(encode));
        std::printf("  %-18s  %9.3f ms  %8.1f MB/s\n", "decode, table", decTable * 1e3, rate(decTable));
        std::printf("  %-18s  %9.3f ms  %8.1f MB/s", "decode, bit by bit", decBit * 1e3, rate(decBit));
        if (raw) std::printf("\n");
        else std::printf("   table is %.1fx faster\n", decBit / decTable);

        const bool ok = tableStatus == cd::Status::Ok && bitStatus == cd::Status::Ok
                        && table == data && bitwise == data;
        std::printf("  %-18s  %s\n", "round trip", ok ? "ok, both decoders" : "FAILED");
        if (!ok) fail(shown(in), "did not survive the round trip");
        return ok;
    }
}

int cli::run(int argc, char** argv){
    const std::vector<std::string> args(argv + (argc > 0 ? 1 : 0), argv + argc);
    if (args.empty()){
        std::fputs(USAGE, stderr);
        return 2;
    }

    Options o;
    std::string err;
    if (!parseArgs(args, o, err)){
        std::fprintf(stderr, "%s: %s\n(%s help lists the commands and options)\n", PROG, err.c_str(), PROG);
        return 2;
    }
    if (o.cmd == Command::help){
        std::fputs(USAGE, stdout);
        return 0;
    }

    /* reading a terminal would sit there waiting for typing, and an archive on a terminal is noise */
    const bool readsStdin = std::find(o.files.begin(), o.files.end(), "-") != o.files.end();
    if (readsStdin && ::isatty(STDIN_FILENO) && !o.force){
        std::fprintf(stderr, "%s: stdin is a terminal, give a file or pipe the data in\n", PROG);
        return 2;
    }
    const bool writesStdout = o.toStdout || o.output == "-" || (readsStdin && o.output.empty());
    if (o.cmd == Command::compress && writesStdout && ::isatty(STDOUT_FILENO) && !o.force){
        std::fprintf(stderr, "%s: not writing an archive to a terminal, redirect it or use -o (-f writes it anyway)\n", PROG);
        return 2;
    }

    Totals totals;
    int failed = 0;
    bool first = true; /* info and bench put a blank line between reports */
    for (const std::string& f : o.files){
        bool ok = true;
        switch (o.cmd){
            case Command::compress:
            case Command::decompress: ok = convertOne(o, f, totals); break;
            case Command::test:       ok = testOne(o, f); break;
            case Command::info:       ok = infoOne(o, f, first); break;
            case Command::bench:      ok = benchOne(o, f, first); break;
            case Command::help:       break;
        }
        if (ok) first = false;
        else ++failed;
    }

    const bool converts = o.cmd == Command::compress || o.cmd == Command::decompress;
    if (converts && !o.quiet && o.files.size() > 1){
        const bool compress = o.cmd == Command::compress;
        const std::string failures = failed ? ", " + std::to_string(failed) + " failed" : "";
        std::fprintf(stderr, "%d file%s  %s -> %s  %s%s\n", totals.files, totals.files == 1 ? "" : "s",
                     cli::humanSize(compress ? totals.original : totals.archive).c_str(),
                     cli::humanSize(compress ? totals.archive : totals.original).c_str(),
                     percent(totals.archive, totals.original).c_str(), failures.c_str());
    }
    return failed ? 1 : 0;
}
