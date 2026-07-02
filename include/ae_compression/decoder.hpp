/*
 * Copyright 2026 Aethernet Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#ifndef AE_COMPRESSION_DECODER_HPP_
#define AE_COMPRESSION_DECODER_HPP_

#include <stdexcept>
#include <vector>

#include "ae_compression/model.hpp"

namespace ae::compression {

class DecodeError : public std::runtime_error {
 public:
  explicit DecodeError(char const* message) : std::runtime_error{message} {}
};

namespace detail {

inline void ExpandSymbols(Model const& model, std::vector<Byte>& out,
                          std::vector<Symbol> const& symbols,
                          std::vector<std::uint8_t>& recursion_stack) {
  for (auto symbol : symbols) {
    if (symbol < kLiteralCount) {
      out.push_back(static_cast<Byte>(symbol));
      continue;
    }

    auto const rule_index = static_cast<std::size_t>(symbol - kFirstRuleSymbol);
    if (rule_index >= model.rules.size()) {
      throw DecodeError{"compressed stream references an unknown rule"};
    }
    if (recursion_stack[rule_index] != 0) {
      throw DecodeError{"compressed stream contains a cyclic rule reference"};
    }

    recursion_stack[rule_index] = 1;
    ExpandSymbols(model, out, model.rules[rule_index].symbols, recursion_stack);
    recursion_stack[rule_index] = 0;
  }
}

}  // namespace detail

inline std::vector<Byte> Decompress(Model const& model) {
  std::vector<Byte> out;
  auto recursion_stack = std::vector<std::uint8_t>(model.rules.size(), 0);
  detail::ExpandSymbols(model, out, model.stream, recursion_stack);
  return out;
}

}  // namespace ae::compression

#endif  // AE_COMPRESSION_DECODER_HPP_

