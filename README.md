# swas

A file compressor written in C++17. It uses canonical Huffman coding with code lengths capped at 15 bits. The decoder itself is table-driven w/ Crc32 checks at every archive, and any files that Huffman can't compress are stored raw instead.

```
$ ./swas c Learned.md cli.cpp random.bin
Learned.md -> Learned.md.swas  5.3 KB -> 3.4 KB  63.9%  0.5 ms
cli.cpp -> cli.cpp.swas  27.2 KB -> 15.7 KB  57.6%  0.8 ms
random.bin -> random.bin.swas  200.0 KB -> 200.3 KB  100.1%  0.9 ms  (stored raw)
3 files  232.4 KB -> 219.3 KB  94.3%
```

## Building

Requirmentrs are C++17 compiler, `make` and GoogleTest for the tests and `pkg-config`. The code uses POSIX calls (`mkstemp`, `isatty`, `umask`) and has been built and tested on macOS.

```sh
brew install googletest pkg-config   # tests only

make          # builds ./swas and the test binaries
make test     # builds everything and runs every test suite
make clean
```

The Makefile builds a debug binary (`-g`, no optimisation). An optimised build runs 1.6–2.5× faster in `bench`:

```sh
make clean && make swas CXXFLAGS="-std=c++17 -O2 -Wall -Wextra"
```

## Usage

```
usage: swas <command> [options] [file...]

commands
  compress, c      file -> file.swas
  decompress, d    file.swas -> file
  test, t          decode archives and check their crc and writes nothing
  info, i          prints an archive's header and code lengths
  bench, b         time encode and both decoders on files, check the round trip
  help             this text

options                                                             for
  -o FILE      output name, one input only; - is stdout             c d
  -c           write to stdout, compress takes one input with it    c d
  -f           overwrite outputs, compress .swas files again,       c d
               allow archives to or from a terminal
  --rm         delete each input once its output is written         c d
  -q           say nothing on success                               c d t
  --bitbybit   decode with the bit by bit decoder                   d t
  --codes      also list every symbol's canonical code              i
  -n RUNS      timed runs per step, default as many as fit in 0.2s  b

With no file, or -, reads stdin and writes stdout. Inputs are kept unless --rm.
Exit status: 0 ok, 1 a file failed, 2 bad usage.
```

Some examples:

```sh
./swas c report.txt               # -> report.txt.swas, report.txt is kept
./swas d report.txt.swas          # -> report.txt (refuses if it exists; -f overwrites)
./swas c --rm *.log               # compress each log, delete the originals
./swas d -o copy.txt report.txt.swas
cat big.csv | ./swas c | ./swas d > same.csv
./swas t *.swas                   # verify archives without writing anything
```

`info` reads only the header. With `--codes` it also lists every symbol's canonical code:

```
$ ./swas i --codes abra.txt.swas
abra.txt.swas
  format    huffman
  original  11 B (11 bytes)
  archive   276 B (276 bytes, 273 of them header), 2509.1% of the original
  crc32     17eaf9b7
  symbols   5 of 256, codes 1 to 3 bits
  payload   2.18 bits per byte, padding included

  bits  symbols
     1        1  ##########
     2        0
     3        4  ########################################

  symbol  bits  code
  'a'        1  0
  'b'        3  100
  'c'        3  101
  'd'        3  110
  'r'        3  111
```

`bench` compares Huffman against the entropy of the byte counts, times each step and checks that both decoders reproduce the input. Timings depend on the build and the machine; these are from the default debug build:

```
$ ./swas b -n 5 cli.cpp
cli.cpp  27.2 KB -> 15.7 KB  57.6%
  entropy                 4.498 bits/byte
  huffman                 4.531 bits/byte, 0.033 over the entropy
  encode                  0.387 ms      70.3 MB/s
  decode, table           0.401 ms      67.7 MB/s
  decode, bit by bit      0.520 ms      52.2 MB/s   table is 1.3x faster
  round trip          ok, both decoders
```

### Safety

- **Nothing is overwritten without `-f`.** This includes dangling symlinks, and an input is never used as its own output.
- **Writes are atomic.** Output goes to a temporary file next to the target and is renamed into place only after every byte is written. A full disk or a killed process never leaves half a file under the real name.
- **`--rm` deletes an input only when its output is safe.** When compressing, the new archive must first decode back to the exact input. When decompressing, the CRC has already been checked.
- **Outputs keep the input's permission bits.**
- **Terminals are refused.** `swas` won't read its input from a terminal, where it would sit waiting for typing, or write an archive to one. `-f` overrides both for `compress` and `decompress`.

## How it works

**Compressing** a file goes through six steps:

1. **Count** how often each byte value occurs.
2. **Build code lengths** with Huffman's algorithm. The leaves are sorted once, stably, then merged with the two-queue method, which avoids a heap. On equal weights a leaf is taken before a merged node. This gives the flattest optimal tree, and the same input always gives the same lengths.
3. **Limit lengths to 15 bits.** Codes deeper than 15 are folded up to 15. The resulting overflow in the Kraft sum is then paid back by splitting the deepest shorter leaf, one slot at a time. The order of the lengths is kept: a byte that had a shorter code than another never ends up with a longer one.
4. **Assign canonical codes.** Codes are handed out in order of (length, byte value), so the archive only needs to store the 256 lengths, not the codes themselves.
5. **Write the bits** most significant bit first, zero-padding the last byte. No pad count is stored, because the decoder stops after the original byte count from the header.
6. **Fall back to raw** storage if the Huffman payload would not be smaller than the input.

**Decompressing** has two decoders:

- **Table decoder (the default).** It peeks at the next 9 bits and looks them up in a 512-entry table, which resolves any code of 9 bits or fewer in one step. Codes of 10 to 15 bits are resolved with a range check per length on a 15-bit window.
- **Bit-by-bit decoder** (`--bitbybit`). It grows the code one bit at a time. It exists as a reference and for comparison in `bench`.

**Checks.** Before anything is decoded, the header is validated:

- the magic bytes
- that no unknown flag bits are set
- that every code length is at most 15 and the lengths form a valid prefix code (Kraft sum ≤ 1)
- that the raw flag and the code table agree

Decoding then checks that the payload length matches the byte count. Finally it checks a CRC-32 of the output. This is the same CRC as zlib, gzip and PNG, so the value `info` prints can be checked with any of their tools. Each kind of failure gets its own message:

| Status | Meaning |
|---|---|
| `BadHeader` | not an archive, or the header is corrupt |
| `BadLength` | the stored length does not match the payload |
| `BadCode` | the payload contains a bit sequence that is not a code |
| `Truncated` | the payload ends before the last symbol |
| `BadChecksum` | decoded, but the CRC does not match |

## Archive format

The header is always 273 bytes, and multi-byte integers are little-endian.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic `swas` |
| 4 | 1 | flags: bit 0 set means the payload is stored raw; other bits must be 0 |
| 5 | 8 | original length in bytes |
| 13 | 4 | CRC-32 of the original data |
| 17 | 256 | code length for each byte value 0–255 (0 = byte does not occur, all 0 when raw) |
| 273 | … | payload |

The payload is either the original bytes (raw) or the canonical Huffman codes, most significant bit first, zero-padded to a whole byte. The code table can be rebuilt from the 256 lengths alone.

## Tests

`make test` runs 8 GoogleTest binaries, one for each part of the code: about 8,800 tests in under 10 seconds.

| Binary | Covers |
|---|---|
| `test_bitwriter` | bit packing, count clamping, flush and padding, checked against a reference model |
| `test_bitreader` | reads, peeks, skips, alignment and overrun, checked against a reference model |
| `test_bitio` | writer-to-reader round trips for every width and random streams |
| `test_huffman` | frequency counts, optimal lengths (checked against an independent min-heap Huffman), the 15-bit limiter, canonical codes |
| `test_format` | header layout and validation; CRC-32 against zlib's published values and its error-detection guarantees |
| `test_codec` | exact archives; every error status; every truncation and bit flip of an archive; both decoders must always agree |
| `test_cli` | argument parsing, output names, size formatting, every command end to end, file modes, terminal handling |
| `test_main` | the built `./swas` binary through real shell pipes (uses `$SWAS_BIN` if set, skipped if there is no binary) |

To run one binary, or a subset of its tests:

```sh
./build/bin/test_codec
./build/bin/test_codec --gtest_filter='*Corruption*'
```

## Project layout

```
src/
  BitWriter.{hpp,cpp}   MSB-first bit packing into a byte vector
  BitReader.{hpp,cpp}   the mirror image: read, peek, skip, align, overrun flag
  Huffman.{hpp,cpp}     frequencies, code lengths, 15-bit limiting, canonical codes
  Format.{hpp,cpp}      the 273-byte header and CRC-32
  Codec.{hpp,cpp}       encode, both decoders, status messages
  cli.{hpp,cpp}         argument parsing and the five commands
  main.cpp              calls cli::run
tests/                  one test file per source file, plus test_support.hpp
Learned.md              notes on the theory behind the design
```

## Limitations

- **Whole files are read into memory** before they are processed. There is no streaming.
- **Each byte is coded on its own**, with no model of repeated strings like LZ77. Compression is therefore bounded by the byte entropy: text and source code come out around 60% of their size, and already-compressed or random data is stored raw.
- **Small files grow.** The fixed 273-byte header means files under a few hundred bytes get larger, even when their payload compresses.
- **The table decoder barely beats bit-by-bit in optimised builds.** `BitReader::peek_bits` still gathers bits one at a time, so reading a whole machine word per peek is the obvious next speed-up.
