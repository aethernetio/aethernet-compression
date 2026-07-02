/*
 * Copyright 2026 Aethernet Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#ifndef AE_COMPRESSION_MODEL_HPP_
#define AE_COMPRESSION_MODEL_HPP_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ae::compression {

using Byte = std::uint8_t;
using Symbol = std::uint32_t;

inline constexpr Symbol kLiteralCount = 256;
inline constexpr Symbol kFirstRuleSymbol = kLiteralCount;

struct Rule {
  std::vector<Symbol> symbols;
};

struct Model {
  std::vector<Symbol> stream;
  std::vector<Rule> rules;
};

struct CompressionOptions {
  std::size_t max_seed_length = 16;
  int min_score = 4;
  bool extend_seed = true;
};

struct CompressionStats {
  std::size_t original_size = 0;
  std::size_t top_level_symbols = 0;
  std::size_t rule_count = 0;
  std::size_t total_symbols = 0;
  std::size_t unique_symbols = 0;
  std::size_t estimated_payload_bytes = 0;
  std::size_t estimated_header_bytes = 0;
  std::size_t estimated_total_bytes = 0;
};

}  // namespace ae::compression

#endif  // AE_COMPRESSION_MODEL_HPP_

