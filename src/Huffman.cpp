#include "Huffman.hpp"
#include <algorithm>

hf::FrequencyTable hf::countFrequencies(const uint8_t* data, std::size_t size){

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

hf::FrequencyTable hf::countFrequencies(const std::vector<uint8_t>& data){
    return countFrequencies(data.data(), data.size());
}

struct Node{
    uint64_t weight; /* decides what merges next */
    int left; int right; int sym;
};

hf::CodeLengths hf::buildCodeLengths(const hf::FrequencyTable& freq) {

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
        return lengths;
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

/* len >>>> Hoe many bits each symbol's code has*/
hf::CodeLengths hf::LimitCodeLengths(const hf::CodeLengths& len){
    /* count[l] := how many symbols have an l length code; l < 256 */
    std::array<uint32_t, 256> count{};
    int longest = 0;
    for (int s = 0; s < 256; ++s){
        if (len[s] == 0) continue;
        ++count[len[s]];
        longest = std::max(longest, static_cast<int>(len[s]));
    }
    if (longest <= MAX_CODE_LEN) return len;

    /* shifting the deeper codes to lv 15*/
    for (int l = MAX_CODE_LEN + 1; l <=longest; ++l){
        count[ MAX_CODE_LEN] += count[l];
        count[l] = 0;
    }

    uint32_t kraft = 0;
    for (int l = 1; l <= MAX_CODE_LEN; ++l){
        kraft += count[l] << (MAX_CODE_LEN - l);
    }

    const uint32_t full = 1u << MAX_CODE_LEN; /* 1 << 15 = 1000000000000000 */

    while (kraft > full) {
        --count[ MAX_CODE_LEN]; /* frees 1 slot*/

        for (int l = MAX_CODE_LEN - 1; l > 0; --l){
            if (count[l] != 0){

                --count[l];
                count[ l + 1] += 2;
                break;
            }
        }

        --kraft;
    }

    /* get new lengths*/
    std::vector<int> order;
    order.reserve(256);
    for (int s = 0; s < 256; ++s) {
        if (len[s] != 0) order.push_back(s);
    }

    /* sorting by old code length*/
    std::stable_sort(order.begin(), order.end(),
                     [&len](int a, int b) { return len[a] < len[b]; });



    hf::CodeLengths limited{};
    std::size_t next = 0;


    for (int L = 1; L <= MAX_CODE_LEN; ++L) {
        for (uint32_t k = 0; k < count[L]; ++k) {
            limited[order[next++]] = static_cast<uint8_t>(L);
        }
    }
    return limited;
}


bool hf::lengthsAreValid(const hf::CodeLengths& len){
    uint32_t kraft = 0; /* 2 ^ 15*/

    for (int s = 0; s < 256; ++s){
        if( len[s] > MAX_CODE_LEN) return false;

        if (len[s] != 0) {
            kraft += 1u << (MAX_CODE_LEN - len[s]);
        }
    }
    return (kraft <= 1u << (MAX_CODE_LEN));
}

/* truning lengtsh into actual code.. */
bool hf::buildCanonicalCodes(const hf::CodeLengths& len, hf::CodeTable& codes){
    if (!lengthsAreValid(len)) return {};

    std::array<uint32_t, MAX_CODE_LEN + 1> blCount{};
    for (int s = 0; s < 256; ++s){
        ++blCount[ len[s]];
    }
    blCount[0] = 0;

    /* gets 1st code for each length*/
    std::array<uint32_t, MAX_CODE_LEN + 1 > nextCode{};
    uint32_t code = 0;

    /* essentially take teh 1st code of the prev length and skip the codes of that length ad add 0 at end*/
    for (int l = 1; l <= MAX_CODE_LEN; ++l){
        code = (code + blCount[ l - 1]) << 1;
        nextCode[l] = code;
    }


    CodeTable built{};
    for (int s = 0; s < 256; ++s){
        if (len[ s]){
            built[s] = nextCode[ len[s]]++;
        }
    }

    codes = built;
    return true;
}

