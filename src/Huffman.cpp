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

struct Node{
    uint64_t weight; /* decides what merges next */
    int left; int right; int sym;
};

hf::CodeLengths buildCodeLengths(const hf::FrequencyTable& freq) {

    /* lengths[b] := the number of bits in b's codeword || 0 if b never existed*/
    hf::CodeLengths lengths{};

    /* creates a leaf node for every byte value that actually appears in the input and remembers where each lives */
    std::vector<Node> nodes; /* each node lives here */
    nodes.reserve(511);
    std::vector<int> leaves; /* indicies into nodes */
    leaves.reserve(256);

    for (int s = 0; s < 256; ++s){
        if (freq[s] != 0){
            /* if theres an frequency for symbol s*/
            leaves.push_back(static_cast<int>(nodes.size()));
            nodes.push_back({freq[s], -1, -1, s});
        }
    }

    if (leaves.empty()) return lengths; /* empty input*/
    if (leaves.size() == 1) { /* eg aaaaaaa, teh tree would be a single node w/ no edges */
        lengths[nodes[leaves[0]].sym] = 1;
    }

    /* sorting by weights */
    std::stable_sort(leaves.begin(), leaves.end(), 
                        [&nodes](int a, int b) {return nodes[a].weight < nodes[b].weight; });

    std::vector<int> merged;
    merged.reserve(255);
    /* read positions */
    std::size_t next_leaf = 0;
    std::size_t next_merged = 0;

    auto remaining = [&] {
        return (leaves.size() - next_leaf) + (merged.size() - next_merged);
    };

    auto pop_smallest = [&] {
        bool take_lead;
        if (next_leaf == leaves.size()){
            take_lead = false;
        } else if (next_merged == merged.size()){
            take_lead = true;
        } else {
            take_lead = nodes[ leaves[ next_leaf]].weight <= nodes[ merged[ next_merged]].weight;
        }
        return take_lead ? leaves[ next_leaf++] : merged[ next_merged++];
    };

    /* merge loop*/
    while (remaining() > 1) {
        int a = pop_smallest();
        int b = pop_smallest();

        uint64_t weight = nodes[a].weight + nodes[b].weight;

        int parent = static_cast<int>(nodes.size());
        nodes.push_back({weight, a, b, -1});
        merged.push_back(parent);

    }
    const int root = pop_smallest();

    /* to get each leafs depth*/
    std::vector<std::pair<int, int>> stack; /* node and depth*/
    stack.reserve(256);
    stack.push_back({root, 0});

    while(!stack.empty()){
        const std::pair<int, int> top = stack.back();
        stack.pop_back();

        const Node& n = nodes[ top.first];

        if (n.left < 0){
            lengths[ n.sym] = static_cast<uint8_t>(top.second);
        } else {
            stack.push_back({n.left, top.second + 1});
            stack.push_back({n.right, top.second + 1});
        }
    }

    return lengths;
}