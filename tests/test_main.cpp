// test_main.cpp -- main(): the built swas binary, run as a separate process.
//
// Everything else tests the library in-process; this drives the real
// executable through /bin/sh with real pipes and redirections, so exit codes,
// stdin/stdout plumbing and the argv hand-off in main.cpp are checked as a
// user sees them. The binary is ./swas (make test builds it first) or $SWAS_BIN;
// the suite is skipped when neither exists.
#include "core/Codec.hpp"
#include "core/Format.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

using ts::Bytes;
namespace fs = std::filesystem;

namespace {

std::string quoted(const std::string& s) {
    std::string q = "'";
    for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}

Bytes encoded(const Bytes& in) {
    Bytes out;
    cd::encode(in, out);
    return out;
}

class MainTest : public ::testing::Test {
protected:
    ts::TempDir dir;
    std::string swas;

    void SetUp() override {
        ASSERT_TRUE(dir.ok());
        const char* env = std::getenv("SWAS_BIN");
        const fs::path bin = fs::absolute(env && *env ? env : "swas");
        if (!fs::exists(bin) || ::access(bin.c_str(), X_OK) != 0) GTEST_SKIP() << "no swas binary at " << bin;
        swas = quoted(bin.string());
    }

    std::string at(const std::string& name) const { return dir / name; }
    std::string put(const std::string& name, const Bytes& data) const {
        ts::writeFile(dir / name, data);
        return dir / name;
    }

    // Runs `command` under /bin/sh with $SWAS set to the binary; returns the exit status.
    int sh(const std::string& command) const {
        const std::string full = "SWAS=" + swas + "; " + command;
        const int status = std::system(full.c_str());
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    std::string errText() const { return ts::readText(dir / "err"); }
    std::string errFile() const { return " 2>" + quoted(at("err")); }
};

}  // namespace

TEST_F(MainTest, NoArgumentsExit2WithUsageOnStderr) {
    EXPECT_EQ(sh("\"$SWAS\" >" + quoted(at("out")) + errFile()), 2);
    EXPECT_EQ(errText().rfind("usage: swas", 0), 0u);
    EXPECT_TRUE(ts::readFile(at("out")).empty());
}

TEST_F(MainTest, HelpExits0WithUsageOnStdout) {
    EXPECT_EQ(sh("\"$SWAS\" help >" + quoted(at("out")) + errFile()), 0);
    EXPECT_EQ(ts::readText(at("out")).rfind("usage: swas", 0), 0u);
    EXPECT_TRUE(errText().empty());
}

TEST_F(MainTest, BadUsageExits2) {
    EXPECT_EQ(sh("\"$SWAS\" frobnicate" + errFile()), 2);
    EXPECT_EQ(errText(), "swas: unknown command 'frobnicate'\n(swas help lists the commands and options)\n");
    EXPECT_EQ(sh("\"$SWAS\" c -o" + errFile()), 2);
    EXPECT_EQ(sh("\"$SWAS\" t -c x" + errFile()), 2);
}

TEST_F(MainTest, ArgumentsReachTheCliUnchanged) {
    // a name with spaces and a quote survives the trip through argv
    const std::string in = put("odd name's file", ts::bytesOf("abracadabra"));
    EXPECT_EQ(sh("\"$SWAS\" c -q " + quoted(in) + errFile()), 0) << errText();
    EXPECT_EQ(ts::readFile(in + ".swas"), encoded(ts::bytesOf("abracadabra")));
}

TEST_F(MainTest, PipelineRoundTrip) {
    const Bytes data = ts::makeData(ts::Shape::Text, 300000, 1);
    const std::string in = put("in", data);
    EXPECT_EQ(sh("cat " + quoted(in) + " | \"$SWAS\" c | \"$SWAS\" d >" + quoted(at("out")) + errFile()), 0) << errText();
    EXPECT_TRUE(ts::readFile(at("out")) == data);
}

TEST_F(MainTest, PipedArchiveIsExactlyTheLibraryEncoding) {
    const Bytes data = ts::makeData(ts::Shape::Geometric, 50000, 2);
    const std::string in = put("in", data);
    EXPECT_EQ(sh("\"$SWAS\" c <" + quoted(in) + " >" + quoted(at("out")) + errFile()), 0) << errText();
    EXPECT_EQ(ts::readFile(at("out")), encoded(data));
}

TEST_F(MainTest, EmptyStdinGivesTheEmptyArchive) {
    EXPECT_EQ(sh("\"$SWAS\" c </dev/null >" + quoted(at("out")) + errFile()), 0) << errText();
    EXPECT_EQ(ts::readFile(at("out")), encoded({}));
}

TEST_F(MainTest, FilesRoundTripWithRemoveAndBothDecoders) {
    for (const std::string& decoder : {"", " --bitbybit"}) {
        const Bytes data = ts::makeData(ts::Shape::Fibonacci, 20000, 3);
        const std::string in = put("f", data);
        ASSERT_EQ(sh("\"$SWAS\" c -q --rm " + quoted(in) + errFile()), 0) << errText();
        ASSERT_FALSE(fs::exists(in));
        ASSERT_EQ(sh("\"$SWAS\" d -q --rm" + decoder + " " + quoted(in + ".swas") + errFile()), 0) << errText();
        ASSERT_FALSE(fs::exists(in + ".swas"));
        EXPECT_TRUE(ts::readFile(in) == data) << decoder;
    }
}

TEST_F(MainTest, TestCommandExitStatusFollowsTheArchive) {
    const std::string good = put("good.swas", encoded(ts::bytesOf("hello, hello")));
    Bytes broken = encoded(ts::bytesOf("hello, hello"));
    broken[13] ^= 0x01;   // the crc; the last payload bit may be padding nobody reads
    const std::string bad = put("bad.swas", broken);
    EXPECT_EQ(sh("\"$SWAS\" t -q " + quoted(good) + errFile()), 0) << errText();
    EXPECT_EQ(sh("\"$SWAS\" t -q " + quoted(bad) + errFile()), 1);
    EXPECT_EQ(sh("\"$SWAS\" t -q " + quoted(good) + " " + quoted(bad) + " " + quoted(good) + errFile()), 1);
}

TEST_F(MainTest, MissingFileExits1) {
    EXPECT_EQ(sh("\"$SWAS\" c " + quoted(at("ghost")) + errFile()), 1);
    EXPECT_EQ(errText(), "swas: " + at("ghost") + ": cannot read: No such file or directory\n");
}

TEST_F(MainTest, InfoAndBenchRunOnTheBinary) {
    const std::string in = put("a", ts::bytesOf("abracadabra"));
    const std::string archive = put("a.swas", encoded(ts::bytesOf("abracadabra")));
    EXPECT_EQ(sh("\"$SWAS\" i --codes " + quoted(archive) + " >" + quoted(at("out")) + errFile()), 0) << errText();
    EXPECT_NE(ts::readText(at("out")).find("  crc32     17eaf9b7\n"), std::string::npos);
    EXPECT_EQ(sh("\"$SWAS\" b -n 1 " + quoted(in) + " >" + quoted(at("out")) + errFile()), 0) << errText();
    EXPECT_NE(ts::readText(at("out")).find("ok, both decoders"), std::string::npos);
}

TEST_F(MainTest, DecompressToStdoutConcatenatesInOrder) {
    const std::string a = put("a.swas", encoded(ts::bytesOf("one "))), b = put("b.swas", encoded(ts::bytesOf("two")));
    EXPECT_EQ(sh("\"$SWAS\" d -c " + quoted(a) + " " + quoted(b) + " >" + quoted(at("out")) + errFile()), 0) << errText();
    EXPECT_EQ(ts::readText(at("out")), "one two");
}

class MainShapeTest : public MainTest, public ::testing::WithParamInterface<ts::Shape> {};

TEST_P(MainShapeTest, EveryShapeSurvivesThePipe) {
    const Bytes data = ts::makeData(GetParam(), 12345, 4);
    const std::string in = put("in", data);
    EXPECT_EQ(sh("\"$SWAS\" c <" + quoted(in) + " | \"$SWAS\" d --bitbybit >" + quoted(at("out")) + errFile()), 0) << errText();
    EXPECT_TRUE(ts::readFile(at("out")) == data);
}

INSTANTIATE_TEST_SUITE_P(Shapes, MainShapeTest, ::testing::ValuesIn(ts::allShapes()),
                         [](const auto& info) { return std::string(ts::shapeName(info.param)); });
