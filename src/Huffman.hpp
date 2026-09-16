#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hf{
    using FrequencyTable = std::array<uint64_t, 256>;

    /* to count frequencies.. */
    FrequencyTable countFrequencies(const uint8_t* data, std::size_t size);
    FrequencyTable countFrequencies(const std::vector<uint8_t>& data);
}