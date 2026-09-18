#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Huffman.hpp"

/* formats job is to turn a struct into bytes and vice versa*/


namespace f {
    constexpr std::size_t HEADER_SIZE = 273;
    constexpr uint8_t FLAG_RAW = 0x01;
    constexpr uint8_t MAGIC[4] = {'s', 'w', 'a', 's'};

    struct Header {
        uint8_t flags = 0; /* huffman code or raw*/
        uint64_t orig_len = 0; /* no of bytes in org file*/
        uint32_t crc = 0; /* crc print of original file*/
        hf::CodeLengths lengths{}; 
    };

    /* writes header size bytes to out*/
    void writeHeader(const Header& h, std::vector<uint8_t>& out);

    /* parses the header bytes of data*/
    bool readHeader(const uint8_t* data, std::size_t size, Header& h);


    uint32_t crc32(const uint8_t* data, std::size_t size);
    uint32_t crc32(const std::vector<uint8_t>& data);
}
