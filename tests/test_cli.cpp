// test_cli.cpp -- cli::, the command line.
//
// parseArgs is driven through every command, alias and flag, the flag/command
// table in the usage text, bundling, values, every refusal message, and a
// seeded fuzz that checks the invariants any accepted Options must satisfy.
// outputPath and humanSize are checked against hand-worked and Python-computed
// values. run() is driven end to end in a scratch directory with stdin,
// stdout and stderr redirected to files (and to a pseudo-terminal for the
// "is a terminal" refusals): every command, the messages, exit codes, file
// modes, --rm, -f, and that no temp file is ever left behind.
#include "Codec.hpp"
#include "Format.hpp"
#include "cli.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ostream>
#include <random>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using cli::Command;
using cli::Options;
using ts::Bytes;
namespace fs = std::filesystem;

namespace cli {
void PrintTo(Command c, std::ostream* os) {
    switch (c) {
        case Command::compress:   *os << "compress"; return;
        case Command::decompress: *os << "decompress"; return;
        case Command::test:       *os << "test"; return;
        case Command::info:       *os << "info"; return;
        case Command::bench:      *os << "bench"; return;
        case Command::help:       *os << "help"; return;
    }
    *os << "Command(" << static_cast<int>(c) << ")";
}
}  // namespace cli

namespace {

// ---------------------------------------------------------------------------
// parseArgs helpers
// ---------------------------------------------------------------------------
struct Parsed {
    bool ok = false;
    Options o;
    std::string err;
};

Parsed parse(const std::vector<std::string>& args) {
    Parsed p;
    // junk in every field: parseArgs must start from a clean Options
    p.o.cmd = Command::bench;
    p.o.files = {"stale"};
    p.o.output = "stale";
    p.o.toStdout = p.o.force = p.o.remove = p.o.quiet = p.o.bitbyByte = p.o.codes = true;
    p.o.runs = 42;
    p.ok = cli::parseArgs(args, p.o, p.err);
    return p;
}

std::string refusal(const std::vector<std::string>& args) {
    const Parsed p = parse(args);
    EXPECT_FALSE(p.ok);
    return p.err;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// run() harness: stdin from a file (or a given fd), stdout and stderr to files
// ---------------------------------------------------------------------------
struct Outcome {
    int code = -1;
    Bytes out;
    std::string err;
    std::string text() const { return std::string(out.begin(), out.end()); }
};

Outcome runCli(const ts::TempDir& io, const std::vector<std::string>& args, const Bytes& stdinData = {},
           int stdinFd = -1, int stdoutFd = -1, bool noArgv0 = false) {
    const std::string inPath = io / "stdin", outPath = io / "stdout", errPath = io / "stderr";
    ts::writeFile(inPath, stdinData);

    std::fflush(stdout);
    std::fflush(stderr);
    const int saved[3] = {::dup(0), ::dup(1), ::dup(2)};
    const int in = stdinFd >= 0 ? stdinFd : ::open(inPath.c_str(), O_RDONLY);
    const int out = stdoutFd >= 0 ? stdoutFd : ::open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const int err = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ::dup2(in, 0);
    ::dup2(out, 1);
    ::dup2(err, 2);
    if (stdinFd < 0) ::close(in);
    if (stdoutFd < 0) ::close(out);
    ::close(err);
    std::clearerr(stdin);

    std::vector<std::string> storage = {"swas"};
    storage.insert(storage.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& s : storage) argv.push_back(s.data());
    argv.push_back(nullptr);

    Outcome r;
    r.code = noArgv0 ? cli::run(0, argv.data()) : cli::run(static_cast<int>(storage.size()), argv.data());

    std::fflush(stdout);
    std::fflush(stderr);
    for (int fd = 0; fd < 3; ++fd) {
        ::dup2(saved[fd], fd);
        ::close(saved[fd]);
    }
    std::clearerr(stdin);

    if (stdoutFd < 0) r.out = ts::readFile(outPath);
    r.err = ts::readText(errPath);
    return r;
}

// A pseudo-terminal: its slave end makes isatty() true.
struct Pty {
    int master = -1, slave = -1;
    Pty() {
        master = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (master < 0 || ::grantpt(master) != 0 || ::unlockpt(master) != 0) return;
        const char* name = ::ptsname(master);
        if (name) slave = ::open(name, O_RDWR | O_NOCTTY);
    }
    ~Pty() {
        if (slave >= 0) ::close(slave);
        if (master >= 0) ::close(master);
    }
    bool ok() const { return slave >= 0 && ::isatty(slave); }
};

mode_t currentUmask() {
    const mode_t m = ::umask(0);
    ::umask(m);
    return m;
}

mode_t modeOf(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 ? (st.st_mode & 0777) : 0;
}

Bytes encoded(const Bytes& in) {
    Bytes out;
    cd::encode(in, out);
    return out;
}

const Bytes& abracadabra() {
    static const Bytes b = ts::bytesOf("abracadabra");
    return b;
}

}  // namespace

// ===========================================================================
// parseArgs: commands
// ===========================================================================
TEST(ParseArgsTest, NoArgumentsIsMissingCommand) {
    EXPECT_EQ(refusal({}), "missing command");
}

struct CommandName {
    std::string name;
    Command cmd;
};

class CommandNameTest : public ::testing::TestWithParam<CommandName> {};

TEST_P(CommandNameTest, NameSelectsTheCommandWithCleanDefaults) {
    const Parsed p = parse({GetParam().name});
    ASSERT_TRUE(p.ok) << p.err;
    EXPECT_EQ(p.o.cmd, GetParam().cmd);
    EXPECT_EQ(p.o.files, std::vector<std::string>{"-"}) << "no file means stdin";
    EXPECT_TRUE(p.o.output.empty());
    EXPECT_FALSE(p.o.toStdout);
    EXPECT_FALSE(p.o.force);
    EXPECT_FALSE(p.o.remove);
    EXPECT_FALSE(p.o.quiet);
    EXPECT_FALSE(p.o.bitbyByte);
    EXPECT_FALSE(p.o.codes);
    EXPECT_EQ(p.o.runs, 0);
}

TEST_P(CommandNameTest, FilesAreKeptInOrder) {
    const Parsed p = parse({GetParam().name, "b", "a", "c"});
    ASSERT_TRUE(p.ok) << p.err;
    EXPECT_EQ(p.o.files, (std::vector<std::string>{"b", "a", "c"}));
}

INSTANTIATE_TEST_SUITE_P(Names, CommandNameTest,
                         ::testing::Values(CommandName{"compress", Command::compress}, CommandName{"c", Command::compress},
                                           CommandName{"decompress", Command::decompress},
                                           CommandName{"d", Command::decompress}, CommandName{"x", Command::decompress},
                                           CommandName{"test", Command::test}, CommandName{"t", Command::test},
                                           CommandName{"info", Command::info}, CommandName{"i", Command::info},
                                           CommandName{"bench", Command::bench}, CommandName{"b", Command::bench}),
                         [](const auto& info) { return info.param.name; });

TEST(ParseArgsTest, HelpSpellingsSelectHelpAndIgnoreTheRest) {
    for (const std::string& name : {"help", "-h", "--help"}) {
        for (const std::vector<std::string>& rest : std::vector<std::vector<std::string>>{{}, {"--bogus"}, {"-o"}, {"a", "b"}}) {
            std::vector<std::string> args = {name};
            args.insert(args.end(), rest.begin(), rest.end());
            const Parsed p = parse(args);
            EXPECT_TRUE(p.ok) << name;
            EXPECT_EQ(p.o.cmd, Command::help) << name;
        }
    }
}

TEST(ParseArgsTest, UnknownCommandsAreRefusedByName) {
    for (const std::string& name : {"zip", "C", "Compress", "compres", "comp", "", "-x", "--rm", "-", "cc", "h"}) {
        EXPECT_EQ(refusal({name, "file"}), "unknown command '" + name + "'");
    }
}

TEST(ParseArgsTest, OptionsAreResetBeforeParsing) {
    const Parsed p = parse({"t", "f"});
    ASSERT_TRUE(p.ok);
    EXPECT_EQ(p.o.files, std::vector<std::string>{"f"});
    EXPECT_TRUE(p.o.output.empty());
    EXPECT_FALSE(p.o.codes);
    EXPECT_EQ(p.o.runs, 0);
}

// ===========================================================================
// parseArgs: files and the special arguments
// ===========================================================================
TEST(ParseArgsTest, DashIsStdinAsAFile) {
    const Parsed p = parse({"c", "a", "-"});
    ASSERT_TRUE(p.ok) << p.err;
    EXPECT_EQ(p.o.files, (std::vector<std::string>{"a", "-"}));
}

TEST(ParseArgsTest, EmptyArgumentIsAFileName) {
    const Parsed p = parse({"t", ""});
    ASSERT_TRUE(p.ok) << p.err;
    EXPECT_EQ(p.o.files, std::vector<std::string>{""});
}

TEST(ParseArgsTest, DoubleDashEndsOptions) {
    const Parsed p = parse({"c", "-q", "--", "-f", "--rm", "-", "--"});
    ASSERT_TRUE(p.ok) << p.err;
    EXPECT_TRUE(p.o.quiet);
    EXPECT_FALSE(p.o.force);
    EXPECT_FALSE(p.o.remove);
    EXPECT_EQ(p.o.files, (std::vector<std::string>{"-f", "--rm", "-", "--"}));
}

TEST(ParseArgsTest, HelpFlagAnywhereBeforeDoubleDashWins) {
    for (const std::vector<std::string>& args : std::vector<std::vector<std::string>>{
             {"c", "-h"}, {"c", "a", "--help"}, {"d", "-f", "-h", "-z"}, {"b", "-n", "5", "--help"}, {"t", "-q", "-h"}}) {
        const Parsed p = parse(args);
        EXPECT_TRUE(p.ok);
        EXPECT_EQ(p.o.cmd, Command::help);
    }
}

TEST(ParseArgsTest, HelpFlagAfterDoubleDashIsAFile) {
    const Parsed p = parse({"t", "--", "-h"});
    ASSERT_TRUE(p.ok);
    EXPECT_EQ(p.o.cmd, Command::test);
    EXPECT_EQ(p.o.files, std::vector<std::string>{"-h"});
}

TEST(ParseArgsTest, HelpFlagAfterAnErrorIsTooLate) {
    EXPECT_EQ(refusal({"c", "-z", "-h"}), "unknown option -z");
}

TEST(ParseArgsTest, StdinCanOnlyBeNamedOnce) {
    EXPECT_EQ(refusal({"c", "-", "-"}), "stdin (-) can only be read once");
    EXPECT_EQ(refusal({"t", "a", "-", "b", "-"}), "stdin (-) can only be read once");
}

// ===========================================================================
// parseArgs: each flag, and which commands take it
// ===========================================================================
TEST(ParseArgsTest, EachFlagSetsItsField) {
    EXPECT_TRUE(parse({"c", "-c", "a"}).o.toStdout);
    EXPECT_TRUE(parse({"c", "-f", "a"}).o.force);
    EXPECT_TRUE(parse({"c", "-q", "a"}).o.quiet);
    EXPECT_TRUE(parse({"c", "--rm", "a"}).o.remove);
    EXPECT_TRUE(parse({"d", "--bitbybit", "a"}).o.bitbyByte);
    EXPECT_TRUE(parse({"i", "--codes", "a"}).o.codes);
    EXPECT_EQ(parse({"c", "-o", "out", "a"}).o.output, "out");
    EXPECT_EQ(parse({"b", "-n", "17", "a"}).o.runs, 17);
}

TEST(ParseArgsTest, RepeatedFlagsAreHarmless) {
    const Parsed p = parse({"c", "-f", "-f", "-qq", "a"});
    ASSERT_TRUE(p.ok) << p.err;
    EXPECT_TRUE(p.o.force);
    EXPECT_TRUE(p.o.quiet);
}

TEST(ParseArgsTest, LastOutputWins) {
    const Parsed p = parse({"c", "-o", "first", "-o", "second", "a"});
    ASSERT_TRUE(p.ok) << p.err;
    EXPECT_EQ(p.o.output, "second");
}

// The "for" column of the usage text: which command takes which flag.
struct FlagCase {
    std::string cmd;
    std::vector<std::string> flag;   // the flag and its value, if any
    bool allowed;
};

std::vector<FlagCase> flagTable() {
    const std::vector<std::pair<std::vector<std::string>, std::string>> flags = {
        {{"-o", "out"}, "cd"}, {{"-c"}, "cd"},        {{"-f"}, "cd"},     {{"--rm"}, "cd"},
        {{"-q"}, "cdt"},       {{"--bitbybit"}, "dt"}, {{"--codes"}, "i"}, {{"-n", "3"}, "b"},
    };
    std::vector<FlagCase> cases;
    for (const std::string cmd : {"c", "d", "t", "i", "b", "compress", "decompress", "test", "info", "bench"}) {
        for (const auto& f : flags) cases.push_back({cmd, f.first, f.second.find(cmd[0]) != std::string::npos});
    }
    return cases;
}

class FlagTableTest : public ::testing::TestWithParam<FlagCase> {};

TEST_P(FlagTableTest, FlagIsTakenOnlyByItsCommands) {
    const FlagCase& c = GetParam();
    std::vector<std::string> args = {c.cmd};
    args.insert(args.end(), c.flag.begin(), c.flag.end());
    args.push_back("file");
    const Parsed p = parse(args);
    if (c.allowed) {
        EXPECT_TRUE(p.ok) << p.err;
    } else {
        EXPECT_FALSE(p.ok);
        EXPECT_EQ(p.err, c.flag[0] + " does not apply to " + c.cmd);
    }
}

INSTANTIATE_TEST_SUITE_P(UsageTable, FlagTableTest, ::testing::ValuesIn(flagTable()), [](const auto& info) {
    std::string name = info.param.cmd + "_";
    for (char ch : info.param.flag[0])
        if (std::isalnum(static_cast<unsigned char>(ch))) name += ch;
    return name;
});

// ===========================================================================
// parseArgs: short flag bundles and values
// ===========================================================================
TEST(ParseArgsTest, ShortFlagsBundle) {
    const Parsed p = parse({"d", "-fqc", "a"});
    ASSERT_TRUE(p.ok) << p.err;
    EXPECT_TRUE(p.o.force);
    EXPECT_TRUE(p.o.quiet);
    EXPECT_TRUE(p.o.toStdout);
}

TEST(ParseArgsTest, ValueFlagTakesTheRestOfTheBundle) {
    Parsed p = parse({"c", "-oout.swas", "a"});
    ASSERT_TRUE(p.ok) << p.err;
    EXPECT_EQ(p.o.output, "out.swas");

    p = parse({"c", "-fqox", "a"});
    ASSERT_TRUE(p.ok) << p.err;
    EXPECT_TRUE(p.o.force);
    EXPECT_TRUE(p.o.quiet);
    EXPECT_EQ(p.o.output, "x");

    p = parse({"c", "-ofq", "a"});   // -o swallows "fq": no flags set
    ASSERT_TRUE(p.ok) << p.err;
    EXPECT_EQ(p.o.output, "fq");
    EXPECT_FALSE(p.o.force);

    p = parse({"b", "-n25", "a"});
    ASSERT_TRUE(p.ok) << p.err;
    EXPECT_EQ(p.o.runs, 25);
}

TEST(ParseArgsTest, ValueFlagAtTheEndOfABundleTakesTheNextArgument) {
    const Parsed p = parse({"c", "-fo", "out", "a"});
    ASSERT_TRUE(p.ok) << p.err;
    EXPECT_TRUE(p.o.force);
    EXPECT_EQ(p.o.output, "out");
    EXPECT_EQ(p.o.files, std::vector<std::string>{"a"});
}

TEST(ParseArgsTest, NextArgumentIsTakenAsAValueEvenIfItLooksLikeAFlag) {
    Parsed p = parse({"c", "-o", "-f", "a"});
    ASSERT_TRUE(p.ok) << p.err;
    EXPECT_EQ(p.o.output, "-f");
    EXPECT_FALSE(p.o.force);

    p = parse({"c", "-o", "--", "a"});
    ASSERT_TRUE(p.ok) << p.err;
    EXPECT_EQ(p.o.output, "--");
    EXPECT_EQ(p.o.files, std::vector<std::string>{"a"});
}

TEST(ParseArgsTest, OutputDashMeansStdout) {
    const Parsed p = parse({"d", "-o", "-", "a.swas"});
    ASSERT_TRUE(p.ok) << p.err;
    EXPECT_EQ(p.o.output, "-");
}

TEST(ParseArgsTest, ValueFlagWithoutAValueIsRefused) {
    EXPECT_EQ(refusal({"c", "-o"}), "-o needs a value");
    EXPECT_EQ(refusal({"c", "a", "-fo"}), "-o needs a value");
    EXPECT_EQ(refusal({"b", "-n"}), "-n needs a value");
    EXPECT_EQ(refusal({"t", "-o"}), "-o needs a value") << "the missing value is noticed before -o's command check";
}

TEST(ParseArgsTest, BadFlagInsideABundleIsNamed) {
    EXPECT_EQ(refusal({"c", "-fz", "a"}), "unknown option -z");
    EXPECT_EQ(refusal({"b", "-qn", "5", "a"}), "-q does not apply to b");
    EXPECT_EQ(refusal({"t", "-qc", "a"}), "-c does not apply to t");
}

TEST(ParseArgsTest, UnknownOptionsAreNamed) {
    for (const std::string& opt : {"-x", "-Z", "-1", "-?"}) EXPECT_EQ(refusal({"c", opt, "a"}), "unknown option " + opt);
    for (const std::string& opt : {"--foo", "--RM", "--rm=1", "--bit-by-bit", "---", "--quiet", "--force", "--stdout"})
        EXPECT_EQ(refusal({"c", opt, "a"}), "unknown option " + opt);
}

class RunsValueTest : public ::testing::TestWithParam<std::pair<std::string, int>> {};

TEST_P(RunsValueTest, OnlyWholeNumbersFromOneToAMillion) {
    const std::string& value = GetParam().first;
    const int want = GetParam().second;
    const Parsed p = parse({"b", "-n", value, "a"});
    if (want > 0) {
        ASSERT_TRUE(p.ok) << p.err;
        EXPECT_EQ(p.o.runs, want);
    } else {
        EXPECT_FALSE(p.ok);
        EXPECT_EQ(p.err, "-n wants a whole number of runs from 1 to 1000000, not '" + value + "'");
    }
}

INSTANTIATE_TEST_SUITE_P(
    Values, RunsValueTest,
    ::testing::Values(std::make_pair(std::string("1"), 1), std::make_pair(std::string("2"), 2),
                      std::make_pair(std::string("10"), 10), std::make_pair(std::string("007"), 7),
                      std::make_pair(std::string("999999"), 999999), std::make_pair(std::string("1000000"), 1000000),
                      std::make_pair(std::string("0"), 0), std::make_pair(std::string("-1"), 0),
                      std::make_pair(std::string("-5"), 0), std::make_pair(std::string("1000001"), 0),
                      std::make_pair(std::string("2147483648"), 0),
                      std::make_pair(std::string("99999999999999999999999"), 0), std::make_pair(std::string(""), 0),
                      std::make_pair(std::string("abc"), 0), std::make_pair(std::string("5x"), 0),
                      std::make_pair(std::string("1.5"), 0), std::make_pair(std::string("1e3"), 0),
                      std::make_pair(std::string("0x10"), 0)));

// ===========================================================================
// parseArgs: combinations that are refused
// ===========================================================================
TEST(ParseArgsTest, OutputAndStdoutTogetherAreRefused) {
    EXPECT_EQ(refusal({"c", "-o", "x", "-c", "a"}), "-o and -c both say where the output goes, give one");
    EXPECT_EQ(refusal({"d", "-c", "-o", "-", "a"}), "-o and -c both say where the output goes, give one");
}

TEST(ParseArgsTest, OutputTakesOneInput) {
    EXPECT_EQ(refusal({"c", "-o", "x", "a", "b"}), "-o takes one input file, got 2");
    EXPECT_EQ(refusal({"d", "-o", "x", "a", "b", "c"}), "-o takes one input file, got 3");
}

TEST(ParseArgsTest, CompressToStdoutTakesOneInput) {
    EXPECT_EQ(refusal({"c", "-c", "a", "b"}), "-c compresses one file at a time, got 2");
}

TEST(ParseArgsTest, DecompressToStdoutTakesManyInputs) {
    const Parsed p = parse({"d", "-c", "a.swas", "b.swas", "c.swas"});
    ASSERT_TRUE(p.ok) << p.err;
    EXPECT_EQ(p.o.files.size(), 3u);
}

TEST(ParseArgsTest, RemoveNeedsAnOutputFile) {
    EXPECT_EQ(refusal({"c", "--rm", "-c", "a"}), "--rm needs an output file, not stdout");
    EXPECT_EQ(refusal({"d", "--rm", "-o", "-", "a.swas"}), "--rm needs an output file, not stdout");
}

TEST(ParseArgsTest, RemoveWithStdinIsAccepted) {
    const Parsed p = parse({"c", "--rm", "-o", "out.swas"});   // reads stdin, nothing to remove
    ASSERT_TRUE(p.ok) << p.err;
    EXPECT_TRUE(p.o.remove);
}

TEST(ParseArgsTest, ChecksRunInTheDocumentedOrder) {
    EXPECT_EQ(refusal({"c", "-o", "x", "-c", "-", "-"}), "stdin (-) can only be read once");
    EXPECT_EQ(refusal({"c", "-o", "x", "-c", "a", "b"}), "-o and -c both say where the output goes, give one");
    EXPECT_EQ(refusal({"c", "--rm", "-o", "-", "a", "b"}), "-o takes one input file, got 2");
    EXPECT_EQ(refusal({"c", "--rm", "-c", "a", "b"}), "-c compresses one file at a time, got 2");
}

// ===========================================================================
// parseArgs: seeded fuzz over the whole vocabulary. Whatever is accepted must
// satisfy every rule; whatever is refused must say why.
// ===========================================================================
class ParseArgsFuzzTest : public ::testing::TestWithParam<int> {};

TEST_P(ParseArgsFuzzTest, AcceptedOptionsAlwaysMakeSense) {
    static const std::vector<std::string> commands = {"c", "d", "x", "t", "i", "b", "compress", "bench", "help", "nope"};
    static const std::vector<std::string> words = {
        "-c", "-f", "-q", "--rm", "--bitbybit", "--codes", "-o", "-n", "5", "0", "out", "-", "--", "a", "b.swas",
        "-fq", "-qc", "-ox", "-n3", "-z", "--bad", "", "-fo", "-h",
    };
    std::mt19937_64 rng(static_cast<uint64_t>(GetParam()) * 1103515245 + 12345);
    for (int t = 0; t < 200; ++t) {
        std::vector<std::string> args = {commands[rng() % commands.size()]};
        const int n = static_cast<int>(rng() % 7);
        for (int i = 0; i < n; ++i) args.push_back(words[rng() % words.size()]);

        const Parsed p = parse(args);
        std::string shown;
        for (const auto& a : args) shown += "'" + a + "' ";
        if (!p.ok) {
            EXPECT_FALSE(p.err.empty()) << shown;
            continue;
        }
        if (p.o.cmd == Command::help) continue;

        const Command c = p.o.cmd;
        const bool writes = c == Command::compress || c == Command::decompress;
        EXPECT_FALSE(p.o.files.empty()) << shown;
        EXPECT_LE(std::count(p.o.files.begin(), p.o.files.end(), "-"), 1) << shown;
        EXPECT_FALSE(!p.o.output.empty() && p.o.toStdout) << shown;
        if (!p.o.output.empty()) EXPECT_EQ(p.o.files.size(), 1u) << shown;
        if (c == Command::compress && p.o.toStdout) EXPECT_EQ(p.o.files.size(), 1u) << shown;
        if (p.o.remove) EXPECT_FALSE(p.o.toStdout || p.o.output == "-") << shown;
        if (p.o.toStdout || p.o.force || p.o.remove || !p.o.output.empty()) EXPECT_TRUE(writes) << shown;
        if (p.o.quiet) EXPECT_TRUE(writes || c == Command::test) << shown;
        if (p.o.bitbyByte) EXPECT_TRUE(c == Command::decompress || c == Command::test) << shown;
        if (p.o.codes) EXPECT_EQ(c, Command::info) << shown;
        if (p.o.runs != 0) EXPECT_EQ(c, Command::bench) << shown;
        EXPECT_GE(p.o.runs, 0) << shown;
        EXPECT_LE(p.o.runs, 1000000) << shown;
    }
}

INSTANTIATE_TEST_SUITE_P(Seeds, ParseArgsFuzzTest, ::testing::Range(0, 100));

// ===========================================================================
// outputPath
// ===========================================================================
TEST(OutputPathTest, SuffixIsDotSwas) {
    EXPECT_STREQ(cli::SUFFIX, ".swas");
}

TEST(OutputPathTest, CompressAlwaysAppendsTheSuffix) {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"a", "a.swas"},           {"a.txt", "a.txt.swas"}, {"dir/a.txt", "dir/a.txt.swas"},
        {"a.swas", "a.swas.swas"}, {"", ".swas"},           {"/abs/path/f", "/abs/path/f.swas"},
        {"dir/", "dir/.swas"},     {".hidden", ".hidden.swas"},
    };
    for (const auto& c : cases) {
        std::string out = "stale";
        EXPECT_TRUE(cli::outputPath(c.first, Command::compress, out)) << c.first;
        EXPECT_EQ(out, c.second);
    }
}

TEST(OutputPathTest, DecompressStripsTheSuffix) {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"a.swas", "a"},           {"a.txt.swas", "a.txt"}, {"dir/a.txt.swas", "dir/a.txt"},
        {"a.swas.swas", "a.swas"}, {"/abs/x.swas", "/abs/x"}, {".hidden.swas", ".hidden"},
        {"x.swas", "x"},           {"swas.swas", "swas"},   {"a b.swas", "a b"},
    };
    for (const auto& c : cases) {
        std::string out = "stale";
        EXPECT_TRUE(cli::outputPath(c.first, Command::decompress, out)) << c.first;
        EXPECT_EQ(out, c.second);
    }
}

TEST(OutputPathTest, DecompressRefusesNamesWithoutAStem) {
    for (const std::string& in : {"a", "a.SWAS", "a.Swas", "a.swa", "a.swas.", "a.swas/", "a.swasx", ".swas",
                                  "dir/.swas", "/.swas", "swas", "", "-"}) {
        std::string out = "untouched";
        EXPECT_FALSE(cli::outputPath(in, Command::decompress, out)) << "'" << in << "'";
        EXPECT_EQ(out, "untouched") << "'" << in << "'";
    }
}

TEST(OutputPathTest, OtherCommandsHaveNoOutputName) {
    for (Command c : {Command::test, Command::info, Command::bench, Command::help}) {
        for (const std::string& in : {"a", "a.swas"}) {
            std::string out = "untouched";
            EXPECT_FALSE(cli::outputPath(in, c, out));
            EXPECT_EQ(out, "untouched");
        }
    }
}

TEST(OutputPathTest, DecompressUndoesCompressForAnyName) {
    std::mt19937_64 rng(1);
    const std::string alphabet = "abcXYZ019._-/ swa";
    for (int t = 0; t < 3000; ++t) {
        std::string name;
        const int n = 1 + static_cast<int>(rng() % 20);
        for (int i = 0; i < n; ++i) name += alphabet[rng() % alphabet.size()];
        if (name.back() == '/') name += 'f';
        std::string archive, back;
        ASSERT_TRUE(cli::outputPath(name, Command::compress, archive));
        ASSERT_TRUE(cli::outputPath(archive, Command::decompress, back)) << name;
        ASSERT_EQ(back, name);
    }
}

// ===========================================================================
// humanSize
// ===========================================================================
class HumanSizeTest : public ::testing::TestWithParam<std::pair<uint64_t, std::string>> {};

TEST_P(HumanSizeTest, MatchesPrintf) {
    EXPECT_EQ(cli::humanSize(GetParam().first), GetParam().second);
}

// expected strings from the same rules in Python ("%.1f", 999.95 moves up a unit)
INSTANTIATE_TEST_SUITE_P(
    Values, HumanSizeTest,
    ::testing::Values(std::make_pair(0ull, "0 B"), std::make_pair(1ull, "1 B"), std::make_pair(512ull, "512 B"),
                      std::make_pair(999ull, "999 B"), std::make_pair(1000ull, "1.0 KB"),
                      std::make_pair(1001ull, "1.0 KB"), std::make_pair(1024ull, "1.0 KB"),
                      std::make_pair(1049ull, "1.0 KB"), std::make_pair(1050ull, "1.1 KB"),
                      std::make_pair(1500ull, "1.5 KB"), std::make_pair(1999ull, "2.0 KB"),
                      std::make_pair(2000ull, "2.0 KB"), std::make_pair(10000ull, "10.0 KB"),
                      std::make_pair(999949ull, "999.9 KB"), std::make_pair(999950ull, "1.0 MB"),
                      std::make_pair(1000000ull, "1.0 MB"), std::make_pair(1048576ull, "1.0 MB"),
                      std::make_pair(1500000ull, "1.5 MB"), std::make_pair(6488666ull, "6.5 MB"),
                      std::make_pair(123456789ull, "123.5 MB"), std::make_pair(999999999ull, "1.0 GB"),
                      std::make_pair(1000000000ull, "1.0 GB"), std::make_pair(1073741824ull, "1.1 GB"),
                      std::make_pair(999949999999ull, "999.9 GB"), std::make_pair(1000000000000ull, "1.0 TB"),
                      std::make_pair(1000000000000000ull, "1000.0 TB"),
                      std::make_pair(18446744073709551615ull, "18446744.1 TB")));

TEST(HumanSizeSweepTest, EveryPowerNeighbourhoodIsWellFormed) {
    std::vector<uint64_t> values;
    for (int k = 0; k < 64; ++k) {
        const uint64_t p = 1ull << k;
        for (uint64_t v : {p - 1, p, p + 1, p + p / 2}) values.push_back(v);
    }
    /* the unit boundaries are powers of ten */
    uint64_t p = 1;
    for (int k = 0; k < 20; ++k, p *= 10)
        for (uint64_t v : {p - 1, p, p + 1, p + p / 2}) values.push_back(v);
    std::mt19937_64 rng(2);
    for (int i = 0; i < 5000; ++i) values.push_back(rng() >> (rng() % 64));

    static const char* const UNITS[] = {"KB", "MB", "GB", "TB"};
    for (uint64_t v : values) {
        const std::string s = cli::humanSize(v);
        if (v < 1000) {
            ASSERT_EQ(s, std::to_string(v) + " B");
            continue;
        }
        double shown = 0;
        char unit[8] = {};
        ASSERT_EQ(std::sscanf(s.c_str(), "%lf %7s", &shown, unit), 2) << s;
        int u = -1;
        for (int k = 0; k < 4; ++k)
            if (std::strcmp(unit, UNITS[k]) == 0) u = k;
        ASSERT_GE(u, 0) << s;
        const double scale = std::pow(1000.0, u + 1);
        ASSERT_NEAR(shown * scale, static_cast<double>(v), 0.05 * scale + 1e-6 * static_cast<double>(v)) << s;
        if (u < 3) ASSERT_LT(shown, 1000.0) << s << ": should have moved up a unit";
        ASSERT_GE(shown, 1.0) << s;
        ASSERT_EQ(s.find('.'), s.find(' ') - 2) << s << ": one decimal place";
    }
}

// ===========================================================================
// run(): end to end, in-process, in a scratch directory
// ===========================================================================
class CliRunTest : public ::testing::Test {
protected:
    ts::TempDir work, io;

    void SetUp() override {
        ASSERT_TRUE(work.ok());
        ASSERT_TRUE(io.ok());
    }

    std::string at(const std::string& name) const { return work / name; }
    std::string put(const std::string& name, const Bytes& data) const {
        ts::writeFile(work / name, data);
        return work / name;
    }
    Outcome run(const std::vector<std::string>& args, const Bytes& in = {}) { return runCli(io, args, in); }
    bool exists(const std::string& name) const { return fs::exists(fs::symlink_status(work / name)); }
    // nothing but these names in the scratch directory: no temp file left behind
    void expectOnly(std::vector<std::string> names) const {
        std::sort(names.begin(), names.end());
        EXPECT_EQ(work.list(), names);
    }
};

// ---------------------------------------------------------------------------
// usage and exit codes
// ---------------------------------------------------------------------------
TEST_F(CliRunTest, NoArgumentsPrintUsageToStderrAndExit2) {
    const Outcome r = run({});
    EXPECT_EQ(r.code, 2);
    EXPECT_EQ(r.err.rfind("usage: swas <command>", 0), 0u) << r.err;
    EXPECT_TRUE(r.out.empty());
}

TEST_F(CliRunTest, ZeroArgcIsTreatedLikeNoArguments) {
    const Outcome r = runCli(io, {"help"}, {}, -1, -1, /*noArgv0=*/true);
    EXPECT_EQ(r.code, 2);
    EXPECT_TRUE(contains(r.err, "usage:"));
}

TEST_F(CliRunTest, HelpPrintsUsageToStdoutAndExits0) {
    for (const std::vector<std::string>& args :
         std::vector<std::vector<std::string>>{{"help"}, {"-h"}, {"--help"}, {"c", "-h"}, {"b", "x", "--help"}}) {
        const Outcome r = run(args);
        EXPECT_EQ(r.code, 0);
        EXPECT_EQ(r.text().rfind("usage: swas <command>", 0), 0u);
        EXPECT_TRUE(contains(r.text(), "Exit status: 0 ok, 1 a file failed, 2 bad usage."));
        EXPECT_TRUE(r.err.empty());
    }
}

TEST_F(CliRunTest, UsageListsEveryCommandAndOption) {
    const std::string u = run({"help"}).text();
    for (const std::string& w : {"compress, c", "decompress, d", "test, t", "info, i", "bench, b", "-o FILE", "-c ",
                                 "-f ", "--rm", "-q ", "--bitbybit", "--codes", "-n RUNS"})
        EXPECT_TRUE(contains(u, w)) << w;
}

TEST_F(CliRunTest, BadUsageExits2WithTheReasonAndAHint) {
    const std::vector<std::pair<std::vector<std::string>, std::string>> cases = {
        {{"zip"}, "unknown command 'zip'"},
        {{"c", "-z"}, "unknown option -z"},
        {{"t", "-o", "x"}, "-o does not apply to t"},
        {{"c", "-", "-"}, "stdin (-) can only be read once"},
        {{"b", "-n", "0"}, "-n wants a whole number of runs from 1 to 1000000, not '0'"},
    };
    for (const auto& c : cases) {
        const Outcome r = run(c.first);
        EXPECT_EQ(r.code, 2) << c.second;
        EXPECT_EQ(r.err, "swas: " + c.second + "\n(swas help lists the commands and options)\n");
        EXPECT_TRUE(r.out.empty());
    }
    expectOnly({});
}

// ---------------------------------------------------------------------------
// compress
// ---------------------------------------------------------------------------
TEST_F(CliRunTest, CompressWritesTheArchiveNextToTheInputAndKeepsTheInput) {
    const std::string in = put("a.txt", abracadabra());
    const Outcome r = run({"c", in});
    EXPECT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(ts::readFile(in), abracadabra());
    EXPECT_EQ(ts::readFile(in + ".swas"), encoded(abracadabra()));
    EXPECT_EQ(r.err.rfind(in + " -> " + in + ".swas  11 B -> 276 B  2509.1%  ", 0), 0u) << r.err;
    EXPECT_TRUE(r.out.empty());
    expectOnly({"a.txt", "a.txt.swas"});
}

TEST_F(CliRunTest, CompressQuietSaysNothing) {
    const std::string in = put("a", abracadabra());
    const Outcome r = run({"c", "-q", in});
    EXPECT_EQ(r.code, 0);
    EXPECT_TRUE(r.err.empty()) << r.err;
    EXPECT_TRUE(r.out.empty());
}

TEST_F(CliRunTest, CompressNotesArchivesStoredRaw) {
    Bytes data;
    for (int s = 0; s < 256; ++s) data.push_back(static_cast<uint8_t>(s));
    const std::string in = put("r", data);
    const Outcome r = run({"c", in});
    EXPECT_EQ(r.code, 0);
    EXPECT_TRUE(contains(r.err, "(stored raw)")) << r.err;
    EXPECT_FALSE(contains(run({"c", "-f", put("h", abracadabra())}).err, "(stored raw)"));
}

TEST_F(CliRunTest, CompressEmptyFile) {
    const std::string in = put("empty", {});
    const Outcome r = run({"c", in});
    EXPECT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(ts::readFile(in + ".swas"), encoded({}));
    EXPECT_TRUE(contains(r.err, "0 B -> 273 B  --  ")) << r.err;
}

TEST_F(CliRunTest, CompressRefusesToOverwriteWithoutForce) {
    const std::string in = put("a", abracadabra());
    put("a.swas", ts::bytesOf("keep me"));
    const Outcome r = run({"c", in});
    EXPECT_EQ(r.code, 1);
    EXPECT_EQ(r.err, "swas: " + in + ".swas: already exists (-f overwrites it)\n");
    EXPECT_EQ(ts::readFile(in + ".swas"), ts::bytesOf("keep me"));
    expectOnly({"a", "a.swas"});
}

TEST_F(CliRunTest, CompressForceOverwrites) {
    const std::string in = put("a", abracadabra());
    put("a.swas", ts::bytesOf("old"));
    const Outcome r = run({"c", "-f", in});
    EXPECT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(ts::readFile(in + ".swas"), encoded(abracadabra()));
    expectOnly({"a", "a.swas"});
}

TEST_F(CliRunTest, DanglingSymlinkCountsAsAnExistingOutput) {
    const std::string in = put("a", abracadabra());
    ASSERT_EQ(::symlink((work / "nowhere").c_str(), (in + ".swas").c_str()), 0);
    const Outcome r = run({"c", in});
    EXPECT_EQ(r.code, 1);
    EXPECT_TRUE(contains(r.err, "already exists")) << r.err;
}

TEST_F(CliRunTest, CompressSkipsSwasFilesUnlessForced) {
    const std::string in = put("x.swas", abracadabra());
    Outcome r = run({"c", in});
    EXPECT_EQ(r.code, 1);
    EXPECT_EQ(r.err, "swas: " + in + ": already ends in .swas, skipped (-f compresses it anyway)\n");
    EXPECT_FALSE(exists("x.swas.swas"));

    r = run({"c", "-f", in});
    EXPECT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(ts::readFile(in + ".swas"), encoded(abracadabra()));
}

TEST_F(CliRunTest, CompressWithOutputName) {
    const std::string in = put("a", abracadabra());
    const Outcome r = run({"c", "-o", at("named.bin"), in});
    EXPECT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(ts::readFile(at("named.bin")), encoded(abracadabra()));
    expectOnly({"a", "named.bin"});
}

TEST_F(CliRunTest, CompressToStdout) {
    const std::string in = put("a", abracadabra());
    for (const std::vector<std::string>& args :
         std::vector<std::vector<std::string>>{{"c", "-c", in}, {"c", "-o", "-", in}, {"c", "-qc", in}}) {
        const Outcome r = run(args);
        EXPECT_EQ(r.code, 0) << r.err;
        EXPECT_EQ(r.out, encoded(abracadabra()));
    }
    expectOnly({"a"});
}

TEST_F(CliRunTest, CompressStdinToStdout) {
    for (const std::vector<std::string>& args : std::vector<std::vector<std::string>>{{"c"}, {"c", "-"}}) {
        const Outcome r = run(args, abracadabra());
        EXPECT_EQ(r.code, 0) << r.err;
        EXPECT_EQ(r.out, encoded(abracadabra()));
        EXPECT_EQ(r.err.rfind("(stdin) -> (stdout)", 0), 0u) << r.err;
    }
}

TEST_F(CliRunTest, CompressStdinToAFile) {
    const Outcome r = run({"c", "-o", at("from-stdin.swas")}, abracadabra());
    EXPECT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(ts::readFile(at("from-stdin.swas")), encoded(abracadabra()));
    EXPECT_EQ(modeOf(at("from-stdin.swas")), 0666 & ~currentUmask());
}

TEST_F(CliRunTest, MissingInputFails) {
    const Outcome r = run({"c", at("ghost")});
    EXPECT_EQ(r.code, 1);
    EXPECT_EQ(r.err, "swas: " + at("ghost") + ": cannot read: No such file or directory\n");
    expectOnly({});
}

TEST_F(CliRunTest, DirectoryInputFails) {
    fs::create_directory(at("sub"));
    const Outcome r = run({"c", at("sub")});
    EXPECT_EQ(r.code, 1);
    EXPECT_EQ(r.err, "swas: " + at("sub") + ": cannot read: is a directory\n");
    expectOnly({"sub"});
}

TEST_F(CliRunTest, InputAndOutputTheSameFileIsRefused) {
    const std::string in = put("a", abracadabra());
    const Outcome r = run({"c", "-f", "-o", in, in});
    EXPECT_EQ(r.code, 1);
    EXPECT_EQ(r.err, "swas: " + in + ": input and output are the same file\n");
    EXPECT_EQ(ts::readFile(in), abracadabra());
}

TEST_F(CliRunTest, HardLinkToTheInputIsTheSameFile) {
    const std::string in = put("a", abracadabra());
    ASSERT_EQ(::link(in.c_str(), at("alias").c_str()), 0);
    const Outcome r = run({"c", "-f", "-o", at("alias"), in});
    EXPECT_EQ(r.code, 1);
    EXPECT_TRUE(contains(r.err, "input and output are the same file")) << r.err;
    EXPECT_EQ(ts::readFile(in), abracadabra());
}

TEST_F(CliRunTest, UnwritableDestinationFailsAndLeavesNothing) {
    const std::string in = put("a", abracadabra());
    const Outcome r = run({"c", "-o", at("no/such/dir/a.swas"), in});
    EXPECT_EQ(r.code, 1);
    EXPECT_EQ(r.err, "swas: " + at("no/such/dir/a.swas") + ": cannot write: No such file or directory\n");
    expectOnly({"a"});
}

TEST_F(CliRunTest, ForcedOutputOntoADirectoryFailsAndCleansUp) {
    const std::string in = put("a", abracadabra());
    fs::create_directory(at("a.swas"));
    const Outcome r = run({"c", "-f", in});
    EXPECT_EQ(r.code, 1);
    EXPECT_TRUE(contains(r.err, "cannot write")) << r.err;
    expectOnly({"a", "a.swas"});
    EXPECT_TRUE(fs::is_directory(at("a.swas")));
}

TEST_F(CliRunTest, RemoveDeletesTheInputOnceTheArchiveIsWritten) {
    const std::string in = put("a", abracadabra());
    const Outcome r = run({"c", "--rm", in});
    EXPECT_EQ(r.code, 0) << r.err;
    expectOnly({"a.swas"});
    Bytes back;
    ASSERT_EQ(cd::decode(ts::readFile(in + ".swas"), back), cd::Status::Ok);
    EXPECT_EQ(back, abracadabra());
}

TEST_F(CliRunTest, RemoveKeepsTheInputWhenAnythingFails) {
    const std::string in = put("a", abracadabra());
    put("a.swas", ts::bytesOf("existing"));
    const Outcome r = run({"c", "--rm", in});
    EXPECT_EQ(r.code, 1);
    EXPECT_EQ(ts::readFile(in), abracadabra());
}

class ModeTest : public CliRunTest, public ::testing::WithParamInterface<mode_t> {};

TEST_P(ModeTest, OutputsCopyTheInputPermissionBits) {
    const std::string in = put("a", abracadabra());
    ASSERT_EQ(::chmod(in.c_str(), GetParam()), 0);
    ASSERT_EQ(run({"c", in}).code, 0);
    EXPECT_EQ(modeOf(in + ".swas"), GetParam());

    ASSERT_EQ(::rename((in + ".swas").c_str(), at("b.swas").c_str()), 0);
    ASSERT_EQ(run({"d", at("b.swas")}).code, 0);
    EXPECT_EQ(modeOf(at("b")), GetParam());
    ::chmod(in.c_str(), 0600);
}

INSTANTIATE_TEST_SUITE_P(Modes, ModeTest, ::testing::Values(0600, 0640, 0644, 0660, 0700, 0755, 0400));

TEST_F(CliRunTest, SeveralFilesGetASummaryLine) {
    const std::string a = put("a", abracadabra()), b = put("b", ts::bytesOf("bbbbbbbbbbbb")), c = put("c", {});
    const Outcome r = run({"c", a, b, c});
    EXPECT_EQ(r.code, 0) << r.err;
    for (const std::string& f : {a, b, c}) EXPECT_TRUE(fs::exists(f + ".swas")) << f;
    EXPECT_TRUE(contains(r.err, "\n3 files  23 B -> ")) << r.err;
}

TEST_F(CliRunTest, OneFailingFileDoesNotStopTheOthers) {
    const std::string a = put("a", abracadabra()), c = put("c", abracadabra());
    const Outcome r = run({"c", a, at("missing"), c});
    EXPECT_EQ(r.code, 1);
    EXPECT_TRUE(fs::exists(a + ".swas"));
    EXPECT_TRUE(fs::exists(c + ".swas"));
    EXPECT_TRUE(contains(r.err, "cannot read")) << r.err;
    EXPECT_TRUE(contains(r.err, "\n2 files  22 B -> 552 B  2509.1%, 1 failed\n")) << r.err;
}

TEST_F(CliRunTest, QuietDropsTheSummaryToo) {
    const std::string a = put("a", abracadabra()), b = put("b", abracadabra());
    const Outcome r = run({"c", "-q", a, b});
    EXPECT_EQ(r.code, 0);
    EXPECT_TRUE(r.err.empty()) << r.err;
}

TEST_F(CliRunTest, OneFileGetsNoSummary) {
    const Outcome r = run({"c", put("a", abracadabra())});
    EXPECT_EQ(std::count(r.err.begin(), r.err.end(), '\n'), 1) << r.err;
}

// ---------------------------------------------------------------------------
// decompress
// ---------------------------------------------------------------------------
TEST_F(CliRunTest, DecompressRestoresTheOriginal) {
    const std::string in = put("a.swas", encoded(abracadabra()));
    const Outcome r = run({"d", in});
    EXPECT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(ts::readFile(at("a")), abracadabra());
    EXPECT_EQ(ts::readFile(in), encoded(abracadabra())) << "the archive is kept";
    EXPECT_EQ(r.err.rfind(in + " -> " + at("a") + "  276 B -> 11 B  2509.1%  ", 0), 0u) << r.err;
}

TEST_F(CliRunTest, DecompressAliasesAgree) {
    for (const std::string& cmd : {"d", "x", "decompress"}) {
        const std::string in = put("z.swas", encoded(abracadabra()));
        std::error_code ec;
        fs::remove(at("z"), ec);
        EXPECT_EQ(run({cmd, in}).code, 0) << cmd;
        EXPECT_EQ(ts::readFile(at("z")), abracadabra()) << cmd;
    }
}

TEST_F(CliRunTest, DecompressNeedsTheSuffixOrAnOutputName) {
    const std::string in = put("archive.bin", encoded(abracadabra()));
    Outcome r = run({"d", in});
    EXPECT_EQ(r.code, 1);
    EXPECT_EQ(r.err, "swas: " + in + ": does not end in .swas, give the output name with -o, or use -c\n");
    expectOnly({"archive.bin"});

    r = run({"d", "-o", at("plain"), in});
    EXPECT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(ts::readFile(at("plain")), abracadabra());

    r = run({"d", "-c", in});
    EXPECT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(r.out, abracadabra());
}

TEST_F(CliRunTest, DecompressRefusesToOverwriteWithoutForce) {
    const std::string in = put("a.swas", encoded(abracadabra()));
    put("a", ts::bytesOf("precious"));
    Outcome r = run({"d", in});
    EXPECT_EQ(r.code, 1);
    EXPECT_TRUE(contains(r.err, "already exists (-f overwrites it)")) << r.err;
    EXPECT_EQ(ts::readFile(at("a")), ts::bytesOf("precious"));

    r = run({"d", "-f", in});
    EXPECT_EQ(r.code, 0);
    EXPECT_EQ(ts::readFile(at("a")), abracadabra());
}

TEST_F(CliRunTest, CorruptArchiveFailsWithTheCodecMessageAndWritesNothing) {
    Bytes bad = encoded(abracadabra());
    bad[13] ^= 1;   // crc
    const std::string in = put("bad.swas", bad);
    const Outcome r = run({"d", in});
    EXPECT_EQ(r.code, 1);
    EXPECT_EQ(r.err, "swas: " + in + ": " + cd::messge(cd::Status::BadChecksum) + "\n");
    expectOnly({"bad.swas"});
}

TEST_F(CliRunTest, EveryDecodeFailureIsReported) {
    const Bytes good = encoded(abracadabra());
    Bytes badCrc = good, trailing = good, truncated = good;
    badCrc[14] ^= 0x80;
    trailing.push_back(0);
    truncated[5] = 13;   // orig_len 13: two symbols past the end
    f::Header inc;
    inc.orig_len = 1;
    inc.lengths['a'] = 1;
    inc.lengths['b'] = 2;
    Bytes badCode;
    f::writeHeader(inc, badCode);
    badCode.insert(badCode.end(), {0xC0, 0x00});

    const std::vector<std::pair<Bytes, cd::Status>> cases = {
        {ts::bytesOf("not an archive"), cd::Status::BadHeader}, {trailing, cd::Status::BadLength},
        {badCode, cd::Status::BadCode},                         {truncated, cd::Status::Truncated},
        {badCrc, cd::Status::BadChecksum},
    };
    for (const auto& c : cases) {
        const std::string in = put("f.swas", c.first);
        for (const std::string& dec : {"", "--bitbybit"}) {
            std::vector<std::string> args = {"d", "-c"};
            if (!dec.empty()) args.push_back(dec);
            args.push_back(in);
            const Outcome r = run(args);
            EXPECT_EQ(r.code, 1);
            EXPECT_EQ(r.err, "swas: " + in + ": " + cd::messge(c.second) + "\n");
            EXPECT_TRUE(r.out.empty());
        }
    }
}

TEST_F(CliRunTest, DecompressSeveralArchivesToStdoutConcatenates) {
    const std::string a = put("a.swas", encoded(ts::bytesOf("first|"))), b = put("b.swas", encoded(ts::bytesOf("second")));
    const Outcome r = run({"d", "-c", a, b});
    EXPECT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(r.text(), "first|second");
}

TEST_F(CliRunTest, DecompressStdinToStdout) {
    const Outcome r = run({"d"}, encoded(abracadabra()));
    EXPECT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(r.out, abracadabra());
}

TEST_F(CliRunTest, DecompressRemoveDeletesTheArchive) {
    const std::string in = put("a.swas", encoded(abracadabra()));
    const Outcome r = run({"d", "--rm", in});
    EXPECT_EQ(r.code, 0) << r.err;
    expectOnly({"a"});
}

TEST_F(CliRunTest, CorruptArchiveIsKeptEvenWithRemove) {
    Bytes bad = encoded(abracadabra());
    bad.back() ^= 0xFF;
    const std::string in = put("a.swas", bad);
    EXPECT_EQ(run({"d", "--rm", in}).code, 1);
    expectOnly({"a.swas"});
}

// Round trips through the command line for every shape, with both decoders.
class CliRoundTripTest : public CliRunTest, public ::testing::WithParamInterface<std::tuple<ts::Shape, int, bool>> {};

TEST_P(CliRoundTripTest, CompressThenDecompressGivesTheFileBack) {
    const Bytes data = ts::makeData(std::get<0>(GetParam()), std::get<1>(GetParam()), 7);
    const std::string in = put("data", data);
    ASSERT_EQ(run({"c", "-q", "--rm", in}).code, 0);
    ASSERT_FALSE(exists("data"));
    std::vector<std::string> args = {"d", "-q", "--rm"};
    if (std::get<2>(GetParam())) args.push_back("--bitbybit");
    args.push_back(in + ".swas");
    const Outcome r = run(args);
    ASSERT_EQ(r.code, 0) << r.err;
    EXPECT_TRUE(ts::readFile(in) == data);
    expectOnly({"data"});
}

INSTANTIATE_TEST_SUITE_P(ShapesSizesDecoders, CliRoundTripTest,
                         ::testing::Combine(::testing::ValuesIn(ts::allShapes()), ::testing::Values(0, 1, 777, 40000),
                                            ::testing::Bool()),
                         [](const auto& info) {
                             return std::string(ts::shapeName(std::get<0>(info.param))) + "_" +
                                    std::to_string(std::get<1>(info.param)) +
                                    (std::get<2>(info.param) ? "_BitByBit" : "_Table");
                         });

// ---------------------------------------------------------------------------
// test
// ---------------------------------------------------------------------------
TEST_F(CliRunTest, TestReportsAGoodArchive) {
    const std::string in = put("a.swas", encoded(abracadabra()));
    const Outcome r = run({"t", in});
    EXPECT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(r.text(), in + ": ok, 11 B\n");
    expectOnly({"a.swas"});
}

TEST_F(CliRunTest, TestQuietPrintsNothing) {
    const Outcome r = run({"t", "-q", put("a.swas", encoded(abracadabra()))});
    EXPECT_EQ(r.code, 0);
    EXPECT_TRUE(r.out.empty());
    EXPECT_TRUE(r.err.empty());
}

TEST_F(CliRunTest, TestWithTheBitByBitDecoder) {
    const Outcome r = run({"t", "--bitbybit", put("a.swas", encoded(abracadabra()))});
    EXPECT_EQ(r.code, 0);
    EXPECT_TRUE(contains(r.text(), ": ok, 11 B\n"));
}

TEST_F(CliRunTest, TestReportsEveryFileAndFailsIfAnyIsBad) {
    Bytes bad = encoded(abracadabra());
    bad[20] = 0xFF;   // a code length of 255
    const std::string good = put("good.swas", encoded(abracadabra()));
    const std::string broken = put("bad.swas", bad);
    const Outcome r = run({"t", good, broken, good});
    EXPECT_EQ(r.code, 1);
    EXPECT_EQ(r.text(), good + ": ok, 11 B\n" + good + ": ok, 11 B\n");
    EXPECT_EQ(r.err, "swas: " + broken + ": " + cd::messge(cd::Status::BadHeader) + "\n");
    expectOnly({"bad.swas", "good.swas"});
}

TEST_F(CliRunTest, TestFromStdin) {
    const Outcome r = run({"t"}, encoded(abracadabra()));
    EXPECT_EQ(r.code, 0);
    EXPECT_EQ(r.text(), "(stdin): ok, 11 B\n");
}

TEST_F(CliRunTest, TestMissingFile) {
    const Outcome r = run({"t", at("nope.swas")});
    EXPECT_EQ(r.code, 1);
    EXPECT_TRUE(contains(r.err, "cannot read: No such file or directory")) << r.err;
}

// ---------------------------------------------------------------------------
// info
// ---------------------------------------------------------------------------
TEST_F(CliRunTest, InfoOnAHuffmanArchive) {
    const std::string in = put("a.swas", encoded(abracadabra()));
    const Outcome r = run({"i", in});
    EXPECT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(r.text(),
              in + "\n"
                   "  format    huffman\n"
                   "  original  11 B (11 bytes)\n"
                   "  archive   276 B (276 bytes, 273 of them header), 2509.1% of the original\n"
                   "  crc32     17eaf9b7\n"
                   "  symbols   5 of 256, codes 1 to 3 bits\n"
                   "  payload   2.18 bits per byte, padding included\n"
                   "\n"
                   "  bits  symbols\n"
                   "     1        1  ##########\n"
                   "     2        0\n"
                   "     3        4  ########################################\n");
}

TEST_F(CliRunTest, InfoCodesListsEveryCanonicalCode) {
    const std::string in = put("a.swas", encoded(abracadabra()));
    const Outcome r = run({"i", "--codes", in});
    EXPECT_EQ(r.code, 0) << r.err;
    const std::string tail =
        "\n"
        "  symbol  bits  code\n"
        "  'a'        1  0\n"
        "  'b'        3  100\n"
        "  'c'        3  101\n"
        "  'd'        3  110\n"
        "  'r'        3  111\n";
    ASSERT_GE(r.text().size(), tail.size());
    EXPECT_EQ(r.text().substr(r.text().size() - tail.size()), tail);
}

TEST_F(CliRunTest, InfoShowsUnprintableSymbolsInHex) {
    const Bytes data = {0x00, 0x00, 0x00, 0x0A, 0x0A, 0x7F, 0x20, 0x7E, 0x00, 0x00};
    const std::string in = put("a.swas", encoded(data));
    const std::string out = run({"i", "--codes", in}).text();
    for (const std::string& name : {"0x00", "0x0a", "0x7f", "' '", "'~'"}) EXPECT_TRUE(contains(out, "  " + name)) << name;
}

TEST_F(CliRunTest, InfoOnARawArchive) {
    const std::string in = put("x.swas", encoded(ts::bytesOf("x")));
    const Outcome r = run({"i", "--codes", in});
    EXPECT_EQ(r.code, 0);
    EXPECT_EQ(r.text(),
              in + "\n"
                   "  format    raw, stored as is (huffman would not have been smaller)\n"
                   "  original  1 B (1 bytes)\n"
                   "  archive   274 B (274 bytes, 273 of them header), 27400.0% of the original\n"
                   "  crc32     8cdc1683\n");
}

TEST_F(CliRunTest, InfoOnAnEmptyFileArchive) {
    const Outcome r = run({"i", put("e.swas", encoded({}))});
    EXPECT_EQ(r.code, 0);
    EXPECT_TRUE(contains(r.text(), "  original  0 B (0 bytes)\n"));
    EXPECT_TRUE(contains(r.text(), "of them header), -- of the original\n")) << r.text();
}

TEST_F(CliRunTest, InfoOnAnEmptyHuffmanArchiveStopsAfterTheHeaderLines) {
    Bytes a;
    f::writeHeader(f::Header{}, a);
    const Outcome r = run({"i", put("e.swas", a)});
    EXPECT_EQ(r.code, 0);
    EXPECT_TRUE(contains(r.text(), "  format    huffman\n"));
    EXPECT_FALSE(contains(r.text(), "symbols"));
}

TEST_F(CliRunTest, InfoSeparatesReportsWithABlankLine) {
    const std::string a = put("a.swas", encoded(abracadabra())), b = put("b.swas", encoded(ts::bytesOf("x")));
    const std::string out = run({"i", a, b}).text();
    EXPECT_TRUE(contains(out, "\n\n" + b + "\n")) << out;
    EXPECT_EQ(out.rfind(a + "\n", 0), 0u);
}

TEST_F(CliRunTest, InfoOnSomethingElseFails) {
    const std::string in = put("nope.swas", ts::bytesOf("hello"));
    const Outcome r = run({"i", in});
    EXPECT_EQ(r.code, 1);
    EXPECT_EQ(r.err, "swas: " + in + ": " + cd::messge(cd::Status::BadHeader) + "\n");
    EXPECT_TRUE(r.out.empty());
}

TEST_F(CliRunTest, InfoFromStdin) {
    const Outcome r = run({"i"}, encoded(abracadabra()));
    EXPECT_EQ(r.code, 0);
    EXPECT_EQ(r.text().rfind("(stdin)\n  format    huffman\n", 0), 0u) << r.text();
}

// ---------------------------------------------------------------------------
// bench
// ---------------------------------------------------------------------------
TEST_F(CliRunTest, BenchReportsEntropyTimingsAndTheRoundTrip) {
    const std::string in = put("a", abracadabra());
    const Outcome r = run({"b", "-n", "2", in});
    EXPECT_EQ(r.code, 0) << r.err;
    const std::string out = r.text();
    EXPECT_EQ(out.rfind(in + "  11 B -> 276 B  2509.1%\n", 0), 0u) << out;
    EXPECT_TRUE(contains(out, "  entropy                 2.040 bits/byte\n")) << out;
    EXPECT_TRUE(contains(out, "  huffman                 2.091 bits/byte, 0.051 over the entropy\n")) << out;
    for (const std::string& step : {"  encode   ", "  decode, table   ", "  decode, bit by bit   "}) EXPECT_TRUE(contains(out, step)) << step;
    EXPECT_TRUE(contains(out, "table is ")) << out;
    EXPECT_TRUE(contains(out, "  round trip          ok, both decoders\n")) << out;
    expectOnly({"a"});
}

TEST_F(CliRunTest, BenchOnRawData) {
    Bytes data;
    for (int s = 0; s < 256; ++s) data.push_back(static_cast<uint8_t>(s));
    const Outcome r = run({"b", "-n", "1", put("r", data)});
    EXPECT_EQ(r.code, 0);
    EXPECT_TRUE(contains(r.text(), "  huffman             stored raw, codes would not have beaten the input\n")) << r.text();
    EXPECT_FALSE(contains(r.text(), "table is"));
    EXPECT_TRUE(contains(r.text(), "ok, both decoders"));
}

TEST_F(CliRunTest, BenchOnAnEmptyFile) {
    const std::string in = put("e", {});
    const Outcome r = run({"b", "-n", "1", in});
    EXPECT_EQ(r.code, 0);
    EXPECT_EQ(r.text(), in + "  empty, nothing to measure\n");
}

TEST_F(CliRunTest, BenchWithoutRunsPicksItsOwnCount) {
    const Outcome r = run({"b", put("a", ts::makeData(ts::Shape::Text, 2000, 1))});
    EXPECT_EQ(r.code, 0);
    EXPECT_TRUE(contains(r.text(), "ok, both decoders"));
}

TEST_F(CliRunTest, BenchSeveralFilesAndAMissingOne) {
    const std::string a = put("a", abracadabra());
    const Outcome r = run({"b", "-n", "1", a, at("missing"), a});
    EXPECT_EQ(r.code, 1);
    EXPECT_TRUE(contains(r.err, "cannot read")) << r.err;
    EXPECT_TRUE(contains(r.text(), "ok, both decoders\n\n" + a)) << "a blank line between reports";
}

// ---------------------------------------------------------------------------
// terminals
// ---------------------------------------------------------------------------
TEST_F(CliRunTest, ReadingStdinFromATerminalIsRefusedForEveryCommand) {
    Pty pty;
    if (!pty.ok()) GTEST_SKIP() << "no pseudo-terminal available";
    for (const std::string& cmd : {"c", "d", "t", "i", "b"}) {
        const Outcome r = runCli(io, {cmd}, {}, pty.slave);
        EXPECT_EQ(r.code, 2) << cmd;
        EXPECT_EQ(r.err, "swas: stdin is a terminal, give a file or pipe the data in\n") << cmd;
    }
}

TEST_F(CliRunTest, WritingAnArchiveToATerminalIsRefused) {
    Pty pty;
    if (!pty.ok()) GTEST_SKIP() << "no pseudo-terminal available";
    const std::string in = put("a", abracadabra());
    for (const std::vector<std::string>& args : std::vector<std::vector<std::string>>{{"c", "-c", in}, {"c", "-o", "-", in}}) {
        const Outcome r = runCli(io, args, {}, -1, pty.slave);
        EXPECT_EQ(r.code, 2);
        EXPECT_EQ(r.err, "swas: not writing an archive to a terminal, redirect it or use -o (-f writes it anyway)\n");
    }
    expectOnly({"a"});
}

TEST_F(CliRunTest, ForceWritesAnArchiveToATerminalAnyway) {
    Pty pty;
    if (!pty.ok()) GTEST_SKIP() << "no pseudo-terminal available";
    const Outcome r = runCli(io, {"c", "-f", "-c", put("a", ts::bytesOf("aaaaaaaa"))}, {}, -1, pty.slave);
    EXPECT_EQ(r.code, 0) << r.err;
}

TEST_F(CliRunTest, DecompressingToATerminalIsAllowed) {
    Pty pty;
    if (!pty.ok()) GTEST_SKIP() << "no pseudo-terminal available";
    const Outcome r = runCli(io, {"d", "-c", put("a.swas", encoded(ts::bytesOf("hi")))}, {}, -1, pty.slave);
    EXPECT_EQ(r.code, 0) << r.err;
}

TEST_F(CliRunTest, TerminalStdoutIsFineWhenCompressingToAFile) {
    Pty pty;
    if (!pty.ok()) GTEST_SKIP() << "no pseudo-terminal available";
    const std::string in = put("a", abracadabra());
    const Outcome r = runCli(io, {"c", "-q", in}, {}, -1, pty.slave);
    EXPECT_EQ(r.code, 0) << r.err;
    EXPECT_TRUE(fs::exists(in + ".swas"));
}
