# Æthernet Compression

`ae-compression` is a tiny-message dictionary compressor for C++20 projects.
It is designed for cases where compression can be slow and expensive on a
desktop or build server, while decompression must stay small enough for a
constrained target device.

The library is standalone: it does not depend on the Æthernet client library.

## Use Cases

- Compress very small repeated messages where general-purpose compressors have
  too much framing overhead.
- Pre-compress binary serialized client state on a powerful machine and ship a
  compact frame to a device that only needs decompression.
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
auto frame = ae::compression::Encode(input);
auto output = ae::compression::Decode(frame);
```

For a target that only needs decompression, include
`ae_compression/decoder.hpp` and `ae_compression/format.hpp`.

## Design

The compressor builds a hierarchy of dictionary rules:

1. Find a repeated seed pattern across the main stream and existing rules.
2. Extend the seed while the longer pattern still repeats.
3. Replace non-overlapping occurrences with a rule symbol.
4. Inline orphan or unprofitable rules and reindex the model.

The compressor is intentionally greedy and slow. That is acceptable for the
intended build-server/offline packing path. The decoder only expands a validated
acyclic rule graph.

## Wire Format

Frames start with `AEC1` and a mode byte:

- `0`: raw payload
- `1`: dictionary-compressed model

The public `Encode` function uses raw fallback when the dictionary frame would
not be smaller than a raw frame. `PackDictionary` can be used when a dictionary
frame is required regardless of size.

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

