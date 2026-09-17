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
    - if the length of code is same as previous one, we increment the $code: code += 1$
    - if teh length is longer
    $$code = (code + 1) << (difference of previous and current lenghts )$$

- a code length can only hold as many symbols as that level has room for
- so once the lenght increases we keep countion but we increase the bit capacity or the length of the code 




## Binary Trees
- merging n items takes n - 1 merges and each merge gets an internal node
- the most leaves well ever have is 256, thus 2 * 256 - 1 = 511

## why stable sort
- it keeps equal weight leaves in symbol order so same input always produces same code lengths
- std::sort gets the order unspecified  whic makes teh input and outputs varried 

## Limiting the code lengths in Huffman
- CPUs process data in fixed sizes, like 8, 16, 32 04 64 bits, if the code is left unbounded with highly squewed data storing codes and left shots become less efficient
- Lookup Table Memory efficiency: 
    - decoders for efficiency should use a pre computed lookup table 
    - for max code length l bits table requires $2^l$ entries
    - a limited 15 bit ceilling should allow the lookup table to fit easily in cpu cashe

## Kraft inquality
- stes a mathamatical limit on the legths of codewords in a prefix code {an encoding system where no single codeword startes with exact sequence of another codeword}
- for any binary prefix code w/ n symbols and codeword lengths $l_1, l_2, ... l_n$ the lengths must satisfy 
$$ \sum_{i=1}^n 2^{-l_i} \le 1$$
- its essentially a space budget inside a binary tree .. 
- in huffman, $ \sum_{i=1}^n 2^{-l_i} \le 1$ turns into $ \sum_{i=1}^n 2^{-l_i} = 1$ as huffman trees are complete binary trees, every internal node has exactly 2 children
- This defines the Matchamatical Constraints for Optimality
----
- Krfat can also be visualised as count slots, w/ cap of 3, there are $2^3 = 8$ possible 3 - bit strings 
- possible slots : 
    - $ 000, 001, 010, 011, 100, 101, 110, 111 $
    - A code claims every slot that starts w/ it ie: "The 1st bits of the slot are the same as that of the code"
    - code 0; of length 1 claims $ 000, 001, 010, 011 $ which are 4 slots
    - code 10 of length 2 claims  $ 100, 101 $ which are 2 slots
    - code 110 of length 3 claims $ 110 $ which is a single slot

- in general a code length of l claims $ 2^{3-l} $ slots, w/ cap of 15 its $ 2^{15-l} $

## 2 sloths cannot overlap
- if 2 codes claim the same slot one of them is the start of teh other
- eg 10 and 101 both claim slot 101. and 10 is starting of 101 
- the decoder wil; get confused as to weather its 10 or 101.. 
- hence code is decodable exactly when each code has its own slot... w/ cap 3 tehre are only 8 slots, so the codes can claim atmost 8 slots 

- THIS IS kraft's INequality 
- $ total Claimes \le total Slots $ whih in case of huffman due to complete binary trees, becomes $ tota Claimes = total Slots $