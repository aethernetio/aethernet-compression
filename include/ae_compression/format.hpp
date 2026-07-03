/*
 * Copyright 2026 Aethernet Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#ifndef AE_COMPRESSION_FORMAT_HPP_
#define AE_COMPRESSION_FORMAT_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include "ae_compression/arithmetic.hpp"
#include "ae_compression/decoder.hpp"
#include "ae_compression/model.hpp"

namespace ae::compression {

class FormatError : public std::runtime_error {
 public:
  explicit FormatError(char const* message) : std::runtime_error{message} {}
};

enum class FrameMode : std::uint8_t {
  kRaw = 0,
  kDictionary = 1,
  kArithmeticDictionary = 2,
};

struct PackOptions {
  bool raw_fallback = true;
};

namespace detail {

inline constexpr std::array<Byte, 4> kMagic = {'A', 'E', 'C', '1'};
inline constexpr Byte kSymbolEscape = 0xff;

inline void WriteUvarint(std::vector<Byte>& out, std::uint64_t value) {
  while (value >= 0x80) {
    out.push_back(static_cast<Byte>((value & 0x7f) | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<Byte>(value));
}

inline std::uint64_t ReadUvarint(std::span<Byte const> data, std::size_t& pos) {
  std::uint64_t value = 0;
  int shift = 0;
  while (pos < data.size()) {
    auto const byte = data[pos++];
    value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      return value;
    }
    shift += 7;
    if (shift >= 64) {
      throw FormatError{"uvarint is too large"};
    }
  }
  throw FormatError{"truncated uvarint"};
}

inline void WriteSymbol(std::vector<Byte>& out, Symbol symbol) {
  if (symbol < kSymbolEscape) {
    out.push_back(static_cast<Byte>(symbol));
    return;
  }

  out.push_back(kSymbolEscape);
  if (symbol == kSymbolEscape) {
    WriteUvarint(out, 0);
  } else {
    WriteUvarint(out, static_cast<std::uint64_t>(symbol - kSymbolEscape));
  }
}

inline Symbol ReadSymbol(std::span<Byte const> data, std::size_t& pos) {
  if (pos >= data.size()) {
    throw FormatError{"truncated symbol"};
  }

  auto const first = data[pos++];
  if (first < kSymbolEscape) {
    return first;
  }

  auto const payload = ReadUvarint(data, pos);
  if (payload == 0) {
    return kSymbolEscape;
  }
  return static_cast<Symbol>(kSymbolEscape + payload);
}

inline void WriteSymbolList(std::vector<Byte>& out,
                            std::vector<Symbol> const& symbols) {
  WriteUvarint(out, symbols.size());
  for (auto symbol : symbols) {
    WriteSymbol(out, symbol);
  }
}

inline std::vector<Symbol> ReadSymbolList(std::span<Byte const> data,
                                          std::size_t& pos) {
  auto const count = ReadUvarint(data, pos);
  auto symbols = std::vector<Symbol>{};
  symbols.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t i = 0; i < count; ++i) {
    symbols.push_back(ReadSymbol(data, pos));
  }
  return symbols;
}

inline void WriteHeader(std::vector<Byte>& out, FrameMode mode) {
  out.insert(out.end(), kMagic.begin(), kMagic.end());
  out.push_back(static_cast<Byte>(mode));
}

inline FrameMode ReadHeader(std::span<Byte const> data, std::size_t& pos) {
  if (data.size() < kMagic.size() + 1) {
    throw FormatError{"frame is too small"};
  }
  for (auto expected : kMagic) {
    if (data[pos++] != expected) {
      throw FormatError{"bad ae-compression frame magic"};
    }
  }
  auto const mode = data[pos++];
  if (mode == static_cast<Byte>(FrameMode::kRaw)) {
    return FrameMode::kRaw;
  }
  if (mode == static_cast<Byte>(FrameMode::kDictionary)) {
    return FrameMode::kDictionary;
  }
  if (mode == static_cast<Byte>(FrameMode::kArithmeticDictionary)) {
    return FrameMode::kArithmeticDictionary;
  }
  throw FormatError{"unknown ae-compression frame mode"};
}

}  // namespace detail

inline std::vector<Byte> PackDictionary(Model const& model) {
  auto out = std::vector<Byte>{};
  detail::WriteHeader(out, FrameMode::kDictionary);
  detail::WriteUvarint(out, model.rules.size());
  for (auto const& rule : model.rules) {
    detail::WriteSymbolList(out, rule.symbols);
  }
  detail::WriteSymbolList(out, model.stream);
  return out;
}

inline std::vector<Byte> PackRaw(std::span<Byte const> data) {
  auto out = std::vector<Byte>{};
  detail::WriteHeader(out, FrameMode::kRaw);
  detail::WriteUvarint(out, data.size());
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

inline std::vector<Byte> PackArithmeticDictionary(Model const& model) {
  auto const flattened = arithmetic::FlattenModel(model);
  auto frequencies = std::vector<std::uint32_t>(
      static_cast<std::size_t>(arithmetic::AlphabetSize(model)), 0);
  for (auto symbol : flattened) {
    if (symbol >= frequencies.size()) {
      throw FormatError{"model symbol exceeds arithmetic alphabet"};
    }
    ++frequencies[symbol];
  }

  auto payload = std::vector<Byte>{};
  if (!flattened.empty()) {
    payload = arithmetic::Encode(flattened,
                                 arithmetic::FrequencyModel{frequencies});
  }

  auto out = std::vector<Byte>{};
  detail::WriteHeader(out, FrameMode::kArithmeticDictionary);
  detail::WriteUvarint(out, model.rules.size());
  detail::WriteUvarint(out, model.stream.size());
  for (auto const& rule : model.rules) {
    detail::WriteUvarint(out, rule.symbols.size());
  }

  auto non_zero_count = std::size_t{0};
  for (auto frequency : frequencies) {
    if (frequency != 0) {
      ++non_zero_count;
    }
  }
  detail::WriteUvarint(out, non_zero_count);
  for (std::size_t i = 0; i < frequencies.size(); ++i) {
    if (frequencies[i] == 0) {
      continue;
    }
    detail::WriteSymbol(out, static_cast<Symbol>(i));
    detail::WriteUvarint(out, frequencies[i]);
  }

  detail::WriteUvarint(out, payload.size());
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

inline Model UnpackDictionary(std::span<Byte const> data) {
  std::size_t pos = 0;
  auto const mode = detail::ReadHeader(data, pos);
  if (mode != FrameMode::kDictionary) {
    throw FormatError{"frame is not dictionary-compressed"};
  }

  auto model = Model{};
  auto const rule_count = detail::ReadUvarint(data, pos);
  model.rules.reserve(static_cast<std::size_t>(rule_count));
  for (std::uint64_t i = 0; i < rule_count; ++i) {
    model.rules.push_back(Rule{detail::ReadSymbolList(data, pos)});
  }
  model.stream = detail::ReadSymbolList(data, pos);
  if (pos != data.size()) {
    throw FormatError{"trailing bytes after dictionary frame"};
  }
  return model;
}

inline Model UnpackArithmeticDictionary(std::span<Byte const> data) {
  std::size_t pos = 0;
  auto const mode = detail::ReadHeader(data, pos);
  if (mode != FrameMode::kArithmeticDictionary) {
    throw FormatError{"frame is not arithmetic dictionary-compressed"};
  }

  auto model = Model{};
  auto const rule_count = detail::ReadUvarint(data, pos);
  auto const stream_size = detail::ReadUvarint(data, pos);
  model.rules.reserve(static_cast<std::size_t>(rule_count));

  auto total_symbol_count = stream_size;
  auto rule_lengths = std::vector<std::uint64_t>{};
  rule_lengths.reserve(static_cast<std::size_t>(rule_count));
  for (std::uint64_t i = 0; i < rule_count; ++i) {
    auto const rule_size = detail::ReadUvarint(data, pos);
    rule_lengths.push_back(rule_size);
    total_symbol_count += rule_size;
  }

  auto frequencies = std::vector<std::uint32_t>(
      static_cast<std::size_t>(kFirstRuleSymbol + rule_count), 0);
  auto const non_zero_count = detail::ReadUvarint(data, pos);
  auto frequency_total = std::uint64_t{0};
  for (std::uint64_t i = 0; i < non_zero_count; ++i) {
    auto const symbol = detail::ReadSymbol(data, pos);
    auto const frequency = detail::ReadUvarint(data, pos);
    if (symbol >= frequencies.size()) {
      throw FormatError{"arithmetic frequency symbol out of range"};
    }
    if (frequency == 0 ||
        frequency > std::numeric_limits<std::uint32_t>::max()) {
      throw FormatError{"bad arithmetic frequency"};
    }
    if (frequencies[symbol] != 0) {
      throw FormatError{"duplicate arithmetic frequency"};
    }
    frequencies[symbol] = static_cast<std::uint32_t>(frequency);
    frequency_total += frequency;
  }
  if (frequency_total != total_symbol_count) {
    throw FormatError{"arithmetic frequency total mismatch"};
  }

  auto const payload_size = detail::ReadUvarint(data, pos);
  if (data.size() - pos != payload_size) {
    throw FormatError{"arithmetic payload size mismatch"};
  }

  auto symbols = std::vector<Symbol>{};
  if (total_symbol_count != 0) {
    symbols = arithmetic::Decode(
        data.subspan(pos, static_cast<std::size_t>(payload_size)),
        static_cast<std::size_t>(total_symbol_count),
        arithmetic::FrequencyModel{std::move(frequencies)});
  }

  auto symbol_pos = std::size_t{0};
  model.stream.assign(symbols.begin(),
                      symbols.begin() + static_cast<std::ptrdiff_t>(stream_size));
  symbol_pos += static_cast<std::size_t>(stream_size);
  for (auto rule_size : rule_lengths) {
    auto const begin = symbols.begin() + static_cast<std::ptrdiff_t>(symbol_pos);
    auto const end = begin + static_cast<std::ptrdiff_t>(rule_size);
    model.rules.push_back(Rule{std::vector<Symbol>{begin, end}});
    symbol_pos += static_cast<std::size_t>(rule_size);
  }
  return model;
}

inline std::vector<Byte> Decode(std::span<Byte const> data) {
  std::size_t pos = 0;
  auto const mode = detail::ReadHeader(data, pos);
  if (mode == FrameMode::kRaw) {
    auto const size = detail::ReadUvarint(data, pos);
    if (data.size() - pos != size) {
      throw FormatError{"raw frame size mismatch"};
    }
    return {data.begin() + static_cast<std::ptrdiff_t>(pos), data.end()};
  }

  if (mode == FrameMode::kArithmeticDictionary) {
    return Decompress(UnpackArithmeticDictionary(data));
  }

  auto model = Model{};
  auto const rule_count = detail::ReadUvarint(data, pos);
  model.rules.reserve(static_cast<std::size_t>(rule_count));
  for (std::uint64_t i = 0; i < rule_count; ++i) {
    model.rules.push_back(Rule{detail::ReadSymbolList(data, pos)});
  }
  model.stream = detail::ReadSymbolList(data, pos);
  if (pos != data.size()) {
    throw FormatError{"trailing bytes after dictionary frame"};
  }
  return Decompress(model);
}

}  // namespace ae::compression

#endif  // AE_COMPRESSION_FORMAT_HPP_
