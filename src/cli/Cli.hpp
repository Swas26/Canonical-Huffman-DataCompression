#pragma once

#include <vector>
#include <cstdint>
#include <string>


namespace cli {

    enum class Command{
        compress,/* encode*/
        decompress,/* decode*/
        test,/* decode and crc checksum*/
        info,/* print header details*/
        bench,/* timed encoding and decoding*/
        help
    };


    constexpr const char* SUFFIX = ".swas";/* compile time cosst, string literal the EXTENSION*/

    struct Options {
        Command cmd = Command::help;
        std::vector<std::string> files;/* input file path*/
        std::string output; /* output path*/
        bool toStdout = false;/* writes to the shell insted of a file*/
        bool force = false;/* overwrites the output file*/
        bool remove = false;/* delet the input files after crc validated*/
        bool quiet = false;/* no progress || stats*/
        bool bitbyByte = false;/* switches to bitBybite encoder*/
        bool codes = false;/* prints the huffman codes*/
        int runs = 0;/* no of repetitions per bench*/
    };


    /* turns an arg into an option*/
    bool parseArgs(const std::vector<std::string>& args, Options& o, std::string& err);

    bool outputPath(const std::string& in, Command cmd, std::string& out);


    /* formats a byte count for display*/
    std::string humanSize(uint64_t bytes);


    /* entry point fo rentire cli*/
    int run(int argc, char** argv);
}