## Bitmasking
- current_byte |= (1 << (7 - bit_count));
- if bit_count is 5; 7 - 5 = 2; && current_byte = 0
- 1 << 2 shifts 1 2 bits to the left
- 00000000 |= 00000100

- if current_byte = 10000000 && bit_count = 2
- 10000000 |= 1 << 5;
- 10000000 |= 00100000;
- 10100000


## BitWriter
- follows MSB first bit order
- write_bits itterates from heighest to lowest bit, following Big Endian Order

