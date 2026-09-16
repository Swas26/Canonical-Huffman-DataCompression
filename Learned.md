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

## CLRS 16.3 HUffman Coding
- we compute the frequency of each character in a file
- we give each character a variable length codeword (binary string)
- frequent characters get short codewords and infrrequent characters get long code words