#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hf{
    using FrequencyTable = std::array<uint64_t, 256>;

    using CodeLengths = std::array<uint8_t, 256>;

    /* to count frequencies.. */
    FrequencyTable countFrequencies(const uint8_t* data, std::size_t size);
    FrequencyTable countFrequencies(const std::vector<uint8_t>& data);

    /* this builds the Huffman tree for the symbols with a non-zero count and returns the depth of each leaf the tree is local to the call: it is born, walked on and freed before this function returns */
    hf::CodeLengths buildCodeLengths(const hf::FrequencyTable& freq);
}