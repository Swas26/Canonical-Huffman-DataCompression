#include "Huffman.hpp"


hf::FrequencyTable countFrequencies(const uint8_t* data, std::size_t size){

    /* freq[b] := how many times byte value b ocures */
    hf::FrequencyTable freq{};

    if (data == nullptr) return freq;
    for (std::size_t i = 0; i < size; ++i){
        ++freq[data[i]];
        /* eg BAB:
            b - 66 - freq[66] := 1
            a - 65 - ffreq[65] := 1
            b - 66 - freq[66] := 2
            */
    }
    return freq;
}

hf::FrequencyTable countFrequencies(const std::vector<uint8_t>& data){
    return countFrequencies(data.data(), data.size());
}