/* Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0. */
#ifndef AE_COMPRESSION_AEGL_PACK64_HPP_
#define AE_COMPRESSION_AEGL_PACK64_HPP_

#include <array>
#include <cstdint>
#include <vector>

#include "ae_compression/aegl/block64.hpp"

namespace ae::compression::aegl {

// Host-only exact LZ parse for the 64-byte, no-prefix profile. Enumerate every
// valid literal length, backward distance, and match length, including overlap.
// This minimizes serialized payload bytes, NOT the size of a grammar or an
// arbitrary compression format. Decoder complexity is independent of this work.
inline std::vector<std::uint8_t> Pack64(
    const std::array<std::uint8_t, kBlockBytes>& input) {
  struct Choice {
    std::uint32_t length;
    std::uint32_t distance;  // Zero denotes literals.
  };
  std::array<std::uint32_t, kBlockBytes + 1> costs;
  std::array<Choice, kBlockBytes> choices;
  costs[kBlockBytes] = 0;
  for (std::uint32_t end = kBlockBytes; end != 0; --end) {
    const auto pos = end - 1;
    costs[pos] = 2 + costs[pos + 1];
    choices[pos] = Choice{1, 0};
    for (std::uint32_t length = 2; length <= kBlockBytes - pos; ++length) {
      const auto cost = 1 + length + costs[pos + length];
      if (cost < costs[pos]) {
        costs[pos] = cost;
        choices[pos] = Choice{length, 0};
      }
    }
    for (std::uint32_t distance = 1; distance <= pos; ++distance) {
      std::uint32_t length = 0;
      while (length < kBlockBytes - pos &&
             input[pos + length] == input[pos + length - distance]) {
        ++length;
        if (length >= 3 && 2 + costs[pos + length] < costs[pos]) {
          costs[pos] = 2 + costs[pos + length];
          choices[pos] = Choice{length, distance};
        }
      }
    }
  }
  std::vector<std::uint8_t> payload;
  payload.reserve(costs[0]);
  for (std::uint32_t pos = 0; pos < kBlockBytes;) {
    const auto choice = choices[pos];
    if (choice.distance == 0) {
      payload.push_back(static_cast<std::uint8_t>(choice.length - 1));
      payload.insert(payload.end(), input.begin() + pos,
                     input.begin() + pos + choice.length);
    } else {
      payload.push_back(static_cast<std::uint8_t>(0x80u + choice.length - 3));
      payload.push_back(static_cast<std::uint8_t>(choice.distance - 1));
    }
    pos += choice.length;
  }
  return payload;
}

}  // namespace ae::compression::aegl
#endif  // AE_COMPRESSION_AEGL_PACK64_HPP_
