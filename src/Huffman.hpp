#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hf{
    using FrequencyTable = std::array<uint64_t, 256>;
    using CodeLengths = std::array<uint8_t, 256>;

    constexpr int MAX_CODE_LEN = 15; /*forces compile time evaluation and gaureenties the value is known */
    using CodeTable = std::array<uint32_t, 256>;


    /* to count frequencies.. */
    FrequencyTable countFrequencies(const uint8_t* data, std::size_t size);
    FrequencyTable countFrequencies(const std::vector<uint8_t>& data);

    /* this builds the Huffman tree for the symbols with a non-zero count and returns the depth of each leaf the tree is local to the call: it is born, walked on and freed before this function returns */
    CodeLengths buildCodeLengths(const hf::FrequencyTable& freq);


    /* assures no code is longer than 15bits */
    CodeLengths LimitCodeLengths(const CodeLengths& len);

    /* true when every length is ,= max code len, and the lengths fit in a binary */
    bool lengthsAreValid(const CodeLengths& len);

    /* building canonical codes off of code lengths*/
    CodeTable buildCanonicalCodes(const CodeLengths& len);
    

}