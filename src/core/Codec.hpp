#pragma once

#include <vector>
/* codec ties everuthing together*/

namespace cd {

    enum class Status {
        Ok,
        BadHeader,/* magic, size, flags or lengths are rejected*/
        BadLength, /* orig_len is incosistent w/ payload*/
        BadCode, /* a bit sequence that matches no sym*/
        Truncated, /* ran out of bts before orig_len*/
        BadChecksum, /* decoded but crc disagrees*/
    };

    /* Encoder reades input bytes and produces and archive in 2 paths : header ( orig len, crc checksum flags ... ) & Payload, ( huffman compressed bits , if compression didnt make the pay7yload smaller :: its just raw bytes ) raw bytes should trigger raw_flag */

    Status encode(const std::vector<uint8_t>& in,std::vector<uint8_t>& out);

    enum class Decoder {
        Table, /* the lookup table: grabs the next 9 bits and gets the sym*/
        BitbyBit, /* fetchec one bit at a time (only for comparison .. )*/
    };

    Status decode(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, Decoder how = Decoder::Table);

    const char* messge(Status s);
}