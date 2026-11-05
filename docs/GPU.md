# AEGL-64: independent tiny-block GPU decoding prototype

## Selection and provenance

This path promotes the allocation-free AEGL literal/copy decoder design, not the
recursive AEC decoder or an adaptive arithmetic model. The original AEGL command
format and minimal decoder are in commit
`977d3050631002a4510da0af02d62b31e1b07e63`, respectively:

- `include/ae_compression/experimental/grammar_lz.hpp`
- `bench/decoder_footprint/decoder_bench.cpp`

The public `block64.hpp` is a fixed-size host/device adaptation of that design.
`Pack64` uses an exact backward dynamic program over every literal run, backward
distance, and match length. It changes encoding decisions, not decoding state.
The selection is based on source-level suitability, NOT measured GPU superiority.
It is not an exhaustive hierarchy search. The existing AEC implementation is
preserved; other experiments remain on their branches.

## Contract and wire representation

Each payload reconstructs **exactly 64 bytes**, starting with an empty history.
The caller supplies payload start and length and valid output storage. Input and
output must not alias. There is no dictionary prefix or cross-block dependency.

| Token | Following bytes | Meaning |
|---|---|---|
| `00..3f` | `token+1` literal bytes | Copy literals to output |
| `80..bd` | one byte `distance-1` | Copy `token-0x80+3` bytes from output history |

All other tokens are rejected by bounds/profile checks. A copy's distance must
be between 1 and the bytes already produced. Copies may overlap their own output
and are evaluated sequentially, exactly as LZ backreferences require. The whole
payload must be consumed when byte 64 is produced. False means discard partial
output. There is no checksum or authentication; rejection of malformed inputs
is not an integrity guarantee.

This is a short-command subset, NOT a new spelling of the full AEGL frame.
Prepending `41 45 47 4c 00 00 40` (AEGL, flags=0, prefix=0, output=64) makes a
valid full AEGL frame. `Decode64` itself consumes only the command payload.
A general AEGL stream containing extended commands or a prefix is not accepted.
No format identifier is stored per payload; the enclosing protocol selects the
profile. Partial final blocks require an external length/padding policy; this
prototype deliberately accepts only complete 64-byte blocks.

A literal-only payload is 65 bytes. The encoder minimizes payload bytes; it does
not include offset-table costs in that optimization. The CUDA example uses
`count+1` 32-bit offsets, hence **4*(count+1) bytes** of address metadata. For 32
blocks this is 132 bytes in addition to payloads, not zero. It is a transparent
reference layout, not a claim that this is the best metadata encoding.

## Execution and memory layout

One lane decodes one block. A warp has 32 independent streams and owns 512 shared
32-bit words (2048 bytes). Logical word `i` of lane `lane` is at
`words[i*32 + lane]`. Sequential writes initialize a new word before reading it;
no prior zero-fill is required. Each lane reads/writes only its own words, so the
decoder needs no warp/block barrier. A later cross-lane consumer must add its own
appropriate synchronization.

The layout maps a lane's history accesses to that lane's shared-memory bank on
hardware with 32 banks of 32-bit words. It does not solve scattered global input
loads, divergent commands, different loop lengths, or load imbalance. See
[NVIDIA's shared-memory guidance](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html#shared-memory-and-memory-banks).

The decoder uses scalar 32-bit positions, lengths, and distances. GPU pointers
remain architecture-sized. No probability array or recursion stack is allocated.
**No extra heap allocation is not zero GPU memory use**: shared output/history,
registers, offsets, and input still consume resources. Compiler register counts,
spills, occupancy, and speed must be measured for the actual consuming kernel.
CPU executable size and CPU stack measurements do not predict these quantities.

The reference kernel launches 128 threads, hence uses 8192 bytes of shared
history per CTA. Tail threads may return early because there is no barrier.
The diagnostic global output is only for roundtrip checking; a fused consumer
would use shared values directly. Neural-weight compressibility and inference
speedup have not been measured.

## Validation

`tests/aegl_block64_tests.cpp` runs without relying on `assert`/`NDEBUG`:

- 5120 roundtrips, including repeated, periodic, binary-alphabet and random data;
- 1152 comparisons against an independent forward shortest-path cost calculation;
- compatibility with a separate interpreter of the original AEGL commands;
- truncated/trailing streams, 50000 randomized malformed-input probes, output
  canaries, all 32 shared columns and cross-column isolation.

The initial local validation passed GCC and Clang host tests and GCC ASan/UBSan.
The work environment had no CUDA toolkit or GPU: local CUDA compilation and
runtime measurements were **not performed**. The `aegl64` CI workflow separately
compiles the CUDA example and emits ptxas resource output; that is still not
runtime GPU validation. Refer to the actual CI run for compilation status, not
this source file as an assertion that a run passed.

On a GPU machine, run the example and `compute-sanitizer --tool memcheck` and
`compute-sanitizer --tool racecheck` on it. Compare independently compressed
64-byte blocks against appropriate LZ4/Heatshrink/ANS baselines, counting all
metadata. Report both a standalone decoder and a fused consuming kernel,
including architecture, compiler flags, register count, spills, shared memory,
output throughput, and end-to-end compute time. No such performance results are
claimed here.
