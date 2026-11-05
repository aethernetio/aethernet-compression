/* Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0. */
#ifndef AE_COMPRESSION_AEGL_BLOCK64_HPP_
#define AE_COMPRESSION_AEGL_BLOCK64_HPP_

#include <cstdint>

#if defined(__CUDACC__)
#define AEGL64_INLINE __host__ __device__ __forceinline__
#else
#define AEGL64_INLINE inline
#endif

namespace ae::compression::aegl {

inline constexpr std::uint32_t kBlockBytes = 64;
inline constexpr std::uint32_t kWarpLanes = 32;

struct LinearOutput64 {
  std::uint8_t* bytes;  // Caller owns at least 64 bytes, disjoint from input.
  AEGL64_INLINE std::uint8_t Read(std::uint32_t i) const { return bytes[i]; }
  AEGL64_INLINE void Write(std::uint32_t i, std::uint8_t value) {
    bytes[i] = value;
  }
};

// One lane owns one column in uint32_t words[16][32]. Each Write is sequential
// from byte zero; the first write to a word initializes all its bits. There is
// no read of uninitialized shared memory and no inter-lane synchronization.
// Caller must provide 512 aligned words and lane < 32.
struct WarpOutput64 {
  std::uint32_t* words;
  std::uint32_t lane;
  AEGL64_INLINE std::uint8_t Read(std::uint32_t i) const {
    return static_cast<std::uint8_t>(
        words[(i >> 2) * kWarpLanes + lane] >> ((i & 3u) * 8));
  }
  AEGL64_INLINE void Write(std::uint32_t i, std::uint8_t value) {
    auto* word = words + (i >> 2) * kWarpLanes + lane;
    const auto shift = (i & 3u) * 8;
    *word = shift == 0 ? static_cast<std::uint32_t>(value)
                      : *word | (static_cast<std::uint32_t>(value) << shift);
  }
};

// Decode the short-command subset of an AEGL payload, with no frame header or
// dictionary prefix. Output is exactly 64 bytes. On false, discard partial
// output. Input bounds are checked; caller supplies valid output storage.
// No allocations, recursion, probabilities, or cross-lane operations.
// Overlapping copies are supported, in sequential byte order within this lane.
template <class Output>
AEGL64_INLINE bool Decode64(const std::uint8_t* input,
                           std::uint32_t input_size, Output output) {
  if (input == nullptr || input_size == 0) return false;
  std::uint32_t position = 0;
  std::uint32_t produced = 0;
  while (produced < kBlockBytes) {
    if (position >= input_size) return false;
    const auto token = input[position++];
    if (token < 0x80u) {
      const auto length = static_cast<std::uint32_t>(token) + 1;
      if (length > kBlockBytes - produced || length > input_size - position)
        return false;
      for (std::uint32_t i = 0; i < length; ++i)
        output.Write(produced++, input[position++]);
    } else {
      if (token > 0xbfu || position >= input_size) return false;
      const auto length = static_cast<std::uint32_t>(token - 0x80u) + 3;
      // In this profile distance <= 63, so the varint is exactly one byte.
      const auto distance = static_cast<std::uint32_t>(input[position++]) + 1;
      if (length > kBlockBytes - produced || distance > produced) return false;
      for (std::uint32_t i = 0; i < length; ++i) {
        const auto value = output.Read(produced - distance);
        output.Write(produced++, value);
      }
    }
  }
  return position == input_size;
}

}  // namespace ae::compression::aegl
#undef AEGL64_INLINE
#endif  // AE_COMPRESSION_AEGL_BLOCK64_HPP_
