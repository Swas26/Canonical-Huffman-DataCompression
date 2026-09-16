## Bitmasking
- current_byte |= (1 << (7 - bit_count));
- if bit_count is 5; 7 - 5 = 2; && current_byte = 0
- 1 << 2 shifts 1 2 bits to the left
- 00000000 |= 00000100

- if current_byte = 10000000 && bit_count = 2
- 10000000 |= 1 << 5;
- 10000000 |= 00100000;
- 10100000


## CLRS 16.3 HUffman Coding
- we compute the frequency of each character in a file
- we give each character a variable length codeword (binary string)
- frequent characters get short codewords and infrrequent characters get long code words


- we sort by frequency of characters (increasing) 
- We take 2 characteers of least frequency  essentially a priority queue || min heap tree
- we take 2 smallest characters and make them chindren of a node, whose value if sum of their frequencies
- we consider the new parent as a char, and re sort the frequency list 
- now we again take 2 least frequent characters and build a parent off of them 
- The min frequency count in the tree will always be on the left
- once the tree is finished, we transverse through it and each left edge of a node gets 0, and right edge gets 1
- teh huffman coding for each char would be set of 0s and 1s from root to that leaf node as each character only sits at leaf node
- then we replace the string of characters into the binary list, and we transvers the 0s and 1s on the tree we made, once we get a leafnode we go bakc to root and work though it again 

## Imporvemnet 

- canonical Huffman
- once we've made the tree, and processed variable codes for each character
- we sort them by lengths of code, and then characters
- we walk the sorted symbols, and start form 0, we only care for lenght of tree code now
    - if the length of code is same as previous one, we increment the code: code += 1;
    - if teh length is longer; code += 1 << (difference of previour and current lenght )

- a code length can only hold as many symbols as that level has room for
- so once the lenght increases we keep countion but we increase the bit capacity or the length of the code 




## Binary Trees
- merging n items takes n - 1 merges and each merge gets an internal node
- the most leaves well ever have is 256, thus 2 * 256 - 1 = 511

## why stable sort
- it keeps equal weight leaves in symbol order so same input always produces same code lengths
- std::sort gets the order unspecified  whic makes teh input and outputs varried 