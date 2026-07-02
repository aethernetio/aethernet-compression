# Æthernet Compression

`ae-compression` is an experimental tiny-message dictionary modeler for C++20
projects. It creates a hierarchy of reusable symbols and can encode the
resulting symbol payload with an arithmetic coder adapted from Project Nayuki's
MIT-licensed reference implementation. It is not yet a complete self-contained
bitstream compressor because metadata for the frequency table and rule layout is
still stored separately.

The intended shape is asymmetric: model building may be slow and expensive on a
desktop or build server, while model expansion on a constrained target device
stays small and predictable.

The library is standalone: it does not depend on the Æthernet client library.

## Use Cases

- Analyze very small repeated messages where general-purpose compressors have
  too much framing overhead.
- Build a compact dictionary model for binary serialized client state on a
  powerful machine, then ship a future entropy-coded frame to a device that only
  needs decompression.
- Experiment with dictionary structures before promoting a format into an
  embedded protocol.

## Integration

```cmake
add_subdirectory(aethernet-compression)
target_link_libraries(app PRIVATE ae-compression::ae-compression)
```

```cpp
#include <ae_compression/ae_compression.hpp>

auto input = std::vector<std::uint8_t>{'a', 'b', 'c', 'a', 'b', 'c'};
auto model = ae::compression::Compress(input);
auto stats = ae::compression::Analyze(model, input.size());
auto arithmetic_payload = ae::compression::arithmetic::EncodeModelPayload(model);
auto output = ae::compression::Decompress(model);
```

For a target that only needs model expansion, include
`ae_compression/decoder.hpp`. `ae_compression/format.hpp` currently stores the
dictionary model in a simple self-describing frame; it is useful for tests and
experiments, but it is not the final entropy-coded representation.

## Design

The compressor builds a hierarchy of dictionary rules:

1. Find a repeated seed pattern across the main stream and existing rules.
2. Extend the seed while the longer pattern still repeats.
3. Replace non-overlapping occurrences with a rule symbol.
4. Inline orphan or unprofitable rules and reindex the model.

The model builder is intentionally greedy and slow. That is acceptable for the
intended build-server/offline packing path. The decoder only expands a validated
acyclic rule graph.

## Wire Format

The current experimental frames start with `AEC1` and a mode byte:

- `0`: raw payload
- `1`: serialized dictionary model

The public `Encode` function uses raw fallback when the serialized dictionary
model would not be smaller than a raw frame. This is a convenience wrapper for
testing, not the final compression story. The key outputs today are
`Analyze(model, original_size)`, which estimates entropy-coded size, and
`arithmetic::EncodeModelPayload(model)`, which produces an actual arithmetic
coded payload for the generated symbols.

The arithmetic backend is adapted from
[Project Nayuki's Reference arithmetic coding](https://github.com/nayuki/Reference-arithmetic-coding)
under the MIT license.

## Build And Test

```bash
cmake -S . -B build -DAE_COMPRESSION_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Example packer:

```bash
cmake --build build --target ae-compression-pack
./build/ae-compression-pack input.bin output.aec
```

Repeating-text analysis with zlib level 9 comparison:

```bash
cmake -S . -B build -DAE_COMPRESSION_BUILD_ZLIB_BENCHMARK=ON
cmake --build build --target ae-compression-repeating-benchmark
./build/ae-compression-repeating-benchmark
```

Real-file analysis with zlib level 9 comparison:

```bash
cmake --build build --target ae-compression-file-benchmark
./build/ae-compression-file-benchmark README.md include/ae_compression/compressor.hpp
```

## Naming

Suggested repository name: `aethernet-compression`.

Suggested CMake target and include:

- `ae-compression::ae-compression`
- `#include <ae_compression/ae_compression.hpp>`

Other reasonable repository names if you want a narrower positioning:

- `aethernet-dict-compression`
- `ae-small-compression`
- `aethernet-state-compression`

## License

Apache License 2.0.
