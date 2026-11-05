# Æthernet Compression

Experimental lossless compression with offline encoding and small decoders.
The library is standalone and does not depend on the Æthernet client library.

## GPU reference: AEGL-64

**One CUDA thread decodes one independent block into 64 bytes of shared memory.**
Each thread receives the start and end offsets of its compressed payload. The
32 threads of a warp decode 32 different blocks; they do not cooperate on one
block. Shared history occupies **2 KiB per warp**.

- [Decoder and shared-memory layout](include/ae_compression/aegl/block64.hpp):
  no allocation, recursion, entropy tables, or separate history allocation.
- [Host encoder](include/ae_compression/aegl/pack64.hpp): exact minimum-byte
  LZ parse for this fixed-size, zero-prefix profile.
- [CUDA example](examples/cuda/aegl64.cu): independent per-thread decoding,
  interleaved shared-memory words, and roundtrip verification.
- [Format, provenance, validation, and limitations](docs/GPU.md).

This is a GPU-oriented **prototype**, not a claim of GPU throughput superiority,
measured register usage, or compression of neural-network weights. CPU tests
and CUDA compilation are distinct from runtime GPU validation. The CUDA example
writes results to global memory for verification; it is not a fused inference
benchmark. Compare against LZ4, Heatshrink, and ANS before drawing performance
conclusions.

AEGL-64 reuses the literal/backreference commands from experimental AEGL. The
fixed-size profile removes per-block frame headers and forbids a dictionary
prefix. **The offset table and batch metadata still cost space** and must be
included in compression measurements. A raw 64-byte input can produce a 65-byte
payload; there is no promise that every block shrinks.

## Build and test

```sh
cmake -S . -B build -DAE_COMPRESSION_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The AEGL-64 headers and standalone host test need only C++17:

```sh
c++ -std=c++17 -O2 -Iinclude tests/aegl_block64_tests.cpp -o aegl64-tests
./aegl64-tests
```

With a CUDA toolkit, select the architecture for the intended GPU:

```sh
nvcc -std=c++17 -O3 -arch=sm_80 -Xptxas=-v -Iinclude \
  examples/cuda/aegl64.cu -o aegl64-cuda
./aegl64-cuda
```

The example exits with code 77 when no CUDA device is available. The `aegl64`
workflow compiles CUDA but does not run it on GPU hardware.

## Original AEC API

The existing C++20 dictionary/arithmetic implementation and targets remain
available and unchanged. Include `ae_compression/ae_compression.hpp` and link
`ae-compression::ae-compression`. `Encode` produces a self-contained AEC1
arithmetic-dictionary frame or falls back to raw data; `Decode` reconstructs it.
The recursive model-expansion API is separate from the AEGL-64 GPU path.

The original file and repeating-text examples compare AEC against zlib.
They are not AEGL-64 GPU benchmarks. Full grammar search, contextual entropy
experiments, and historical benchmark workflows remain on the experimental
branches; they are not silently presented as this fixed-block codec.

## License

Apache License 2.0. The existing arithmetic backend includes an adaptation of
Project Nayuki's MIT-licensed reference arithmetic coder; see source notices.
