#include "Codec.hpp"
#include "BitReader.hpp"
#include "BitWriter.hpp"
#include "Huffman.hpp"
#include "Format.hpp"

namespace /* helpers, kinda like static */
{

constexpr int FAST_BITS = 9; /* table is indexed by nnnext 9 bits maybe the same is done in zlib */


/* this is what a table slot would have... ; if len == 0 slot has no ans*/
struct Entry {
    uint8_t sym = 0;
    uint8_t len = 0;
};

/* numbers computed by buildCanonicalCodes are arranges for lookup*/
struct DecodeTable {
    uint32_t count[ hf::MAX_CODE_LEN + 1] = {};/* how many symbols of length l*/
    uint32_t firstCode[ hf::MAX_CODE_LEN + 1] = {}; /* smalles code of same len l*/
    uint32_t firstIndex[hf::MAX_CODE_LEN + 1] = {}; /* index of 1stcode in sorted*/
    uint8_t sorted[ 256] = {};/* symbs in sorted code order*/

    Entry fast[1u << FAST_BITS] = {};/* 2^9 || 512*/
};

DecodeTable buildDecodeTable(const hf::CodeLengths& lengths){
    DecodeTable t;

    /* count the codes of each length*/
    for (int s = 0; s < 256; ++s){
        ++t.count[ lengths[s]];
    }
    t.count[0] = 0;

    uint32_t code = 0, index = 0;

    for( int l = 1; l <= hf::MAX_CODE_LEN; ++l){
        code = (code + t.count[l - 1]) << 1;/* get 1st code and 1st index for each length*/
        t.firstCode[l] = code;
        t.firstIndex[l] = index;
        index += t.count[l];
    }

    uint32_t next[hf::MAX_CODE_LEN + 1] = {};
    for ( int l = 1; l <= hf::MAX_CODE_LEN; ++l){
        next[l] = t.firstIndex[l];/* next[l] here is the next free position in sorted for length l*/
    }


    /* grouping the symbols by lengths and byteorder*/
    for (int s = 0; s < 256; ++s){
        if (lengths[s] != 0){
            t.sorted[next[lengths[s]]++] = static_cast<uint8_t>(s);
        }
    }


    /* populating the fast table*/
    for(int l = 1; l <= FAST_BITS; ++l){
        const uint32_t span = 1u << (FAST_BITS - l);

        for (uint32_t i = 0; i < t.count[l]; ++i){
            const Entry e{ t.sorted[t.firstIndex[l] + i], static_cast<uint8_t>(l)};
            const uint32_t first = (t.firstCode[l] + i) * span;

            for (uint32_t slot = first; slot < first + span; ++slot){
                t.fast[slot] = e;
            }
        }
    }

    return t;
}


/* for codes longer than FAST_BITS, lookup per length range

Assumption: most frequent ones should already havee been incorperated earlier.. */
Entry decodeLong(const DecodeTable& t, uint32_t window){

    /* essentially window holds next 15 bits fo stream*/

    for (int l = FAST_BITS + 1; l <= hf::MAX_CODE_LEN; ++l){
        const uint32_t offset = (window >> (hf::MAX_CODE_LEN - l )) - t.firstCode[l]; /* offset is 1st l bits - first code*/

        if (offset < t.count[l]){
            return Entry { 
                t.sorted[t.firstIndex[l] + offset],
                static_cast<uint8_t>(l)
            };
        }
    }

    return Entry{};
}


cd::Status decodeTable(const DecodeTable& t, BitReader& r, uint64_t sym, std::vector<uint8_t>& out){

    while (out.size() < sym){

        Entry e = t.fast[ r.peek_bits(FAST_BITS)];

        if(e.len == 0){
            e = decodeLong(t, static_cast<uint32_t>(r.peek_bits(hf::MAX_CODE_LEN)));/* empty slot means the code is maybe longer than 9bits, so look into decodelong*/
        }

        if (e.len == 0){
            return r.bits_remaining() < hf::MAX_CODE_LEN ? cd::Status::Truncated : cd::Status::BadCode; /* its still not valid*/
        }

        r.skip_bits(e.len);

        if(!r.ok()){
            return cd::Status::Truncated;
        }

        out.push_back(e.sym);
    }

    return cd::Status::Ok;

}


cd::Status DecodeBitbyBit(const DecodeTable& t, BitReader& r, uint64_t sym, std::vector<uint8_t>& out){

    uint32_t code = 0;
    int len = 0;

    while( out.size() < sym){

        code = (code << 1) | (r.read_bit() ? 1u : 0u);/* intent is to shift the collected bots left by one and put the new bit at lowest pos*/
        ++len;

        if (len > hf::MAX_CODE_LEN) return cd::Status::BadCode;/* no code can be longer than 15*/

        if (!r.ok()) return cd::Status::Truncated;

        const uint32_t offset = code - t.firstCode[len];/* if teh code is of valid len, fist code - len  = pos in that group*/

        if( offset < t.count[len]){
            out.push_back( t.sorted[ t.firstIndex[len] + offset]);
            code = 0;
            len = 0;
        }
    }

    return cd::Status::Ok;
}


cd::Status decodeInto(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, cd::Decoder how){
    f::Header h;

    if (!f::readHeader( in.data(), in.size(), h)) return cd::Status::BadHeader;/* read the header*/


    /* get payload*/
    const uint8_t* payload = in.data() + f::HEADER_SIZE;
    const std::size_t payloadSize = in.size() - f::HEADER_SIZE;

    BitReader r(payload, payloadSize);

    if (h.flags & f::FLAG_RAW){
        /* raw case !huffman*/
        if (h.orig_len != payloadSize) return cd::Status::BadLength;
        out.assign(payload, payload + payloadSize);
        return f::crc32(out) == h.crc ? cd::Status::Ok : cd::Status::BadChecksum;
    }


    if (h.orig_len > static_cast<uint64_t>(payloadSize) * 8){
        return cd::Status::BadLength;
    }

    const DecodeTable t = buildDecodeTable(h.lengths);

    out.clear();

    const cd::Status s = ( how == cd::Decoder::BitbyBit ? DecodeBitbyBit(t, r, h.orig_len, out) : decodeTable(t, r, h.orig_len, out));
    if (s != cd::Status::Ok) return s;

    if ( (r.bits_read() + 7) / 8 != payloadSize){
        return cd::Status::BadLength;
    }

    return f::crc32(out) == h.crc ? cd::Status::Ok : cd::Status::BadChecksum;

}
}

cd::Status cd::encode(const std::vector<uint8_t>& in, std::vector<uint8_t>& out){

    out.clear();

    f::Header h;
    h.orig_len =  in.size();
    h.crc = f::crc32(in);


    /* build huffman*/
    const auto freq =  hf::countFrequencies(in);
    const auto lengths = hf::LimitCodeLengths( hf::buildCodeLengths(freq));


    hf::CodeTable codes{};
    const bool usable = hf::buildCanonicalCodes(lengths,  codes);

    uint64_t bits = 0;

    /* compressed size, !w/ encoding*/
    for (int s = 0; s < 256; ++s){
        bits += freq[s] * lengths[s];
    }

    const uint64_t huffBytes = (bits + 7) / 8;


    /* rawfallback.. */
    if (!usable || huffBytes >= in.size()){

        h.flags |= f::FLAG_RAW;
        h.lengths = {};
        f::writeHeader(h, out);
        out.insert(out.end(), in.begin(), in.end());
        return Status::Ok;
    }


    /* Huffman path*/
    h.lengths = lengths;
    f::writeHeader(h, out);

    BitWriter w(out);
    for (uint8_t b : in){
         w.write_bits(codes[b], lengths[b]);
    }

    w.flush();

    return Status::Ok;
}

cd::Status cd::decode(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, Decoder how){

    out.clear();
    const Status s = decodeInto(in, out, how);

    if (s != Status::Ok) out.clear();
    return s;
}


const char* cd::messge(cd::Status s){
    switch (s){
        case cd::Status::Ok: return "ok";
        case cd::Status::BadHeader: return "not an archive file or t header is corrupt";
        case cd::Status::BadLength: return "stored length does not match the payload";
        case cd::Status::BadCode: return "payload contains a bit sequence that is not a code";
        case cd::Status::Truncated: return "payload ends before the last symbol";
        case cd::Status::BadChecksum: return "checksum dosent match, decoded data is corrupt";
    }
    return "we dont know what happened :)";
}