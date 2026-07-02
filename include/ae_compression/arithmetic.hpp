/*
 * Arithmetic coding backend adapted from:
 *
 * Reference arithmetic coding
 * Copyright (c) Project Nayuki
 * MIT License
 * https://www.nayuki.io/page/reference-arithmetic-coding
 *
 * Adaptation copyright 2026 Aethernet Inc.
 * Licensed under the Apache License, Version 2.0.
 */

#ifndef AE_COMPRESSION_ARITHMETIC_HPP_
#define AE_COMPRESSION_ARITHMETIC_HPP_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include "ae_compression/model.hpp"

namespace ae::compression::arithmetic {

class ArithmeticError : public std::runtime_error {
 public:
  explicit ArithmeticError(char const* message) : std::runtime_error{message} {}
};

class BitWriter {
 public:
  void Write(int bit) {
    if (bit != 0 && bit != 1) {
      throw ArithmeticError{"bit must be 0 or 1"};
    }
    current_byte_ = static_cast<Byte>((current_byte_ << 1) | bit);
    ++bits_filled_;
    if (bits_filled_ == 8) {
      bytes_.push_back(current_byte_);
      current_byte_ = 0;
      bits_filled_ = 0;
    }
  }

  std::vector<Byte> Finish() {
    while (bits_filled_ != 0) {
      Write(0);
    }
    return bytes_;
  }

 private:
  std::vector<Byte> bytes_;
  Byte current_byte_ = 0;
  int bits_filled_ = 0;
};

class BitReader {
 public:
  explicit BitReader(std::span<Byte const> bytes) : bytes_{bytes} {}

  int Read() {
    if (bits_remaining_ == 0) {
      if (pos_ >= bytes_.size()) {
        return -1;
      }
      current_byte_ = bytes_[pos_++];
      bits_remaining_ = 8;
    }
    --bits_remaining_;
    return (current_byte_ >> bits_remaining_) & 1;
  }

 private:
  std::span<Byte const> bytes_;
  std::size_t pos_ = 0;
  Byte current_byte_ = 0;
  int bits_remaining_ = 0;
};

class FrequencyModel {
 public:
  explicit FrequencyModel(std::vector<std::uint32_t> frequencies)
      : frequencies_{std::move(frequencies)} {
    if (frequencies_.empty()) {
      throw ArithmeticError{"frequency model must contain at least one symbol"};
    }

    cumulative_.reserve(frequencies_.size() + 1);
    cumulative_.push_back(0);
    for (auto frequency : frequencies_) {
      if (frequency > std::numeric_limits<std::uint32_t>::max() - total_) {
        throw ArithmeticError{"frequency model total overflow"};
      }
      total_ += frequency;
      cumulative_.push_back(total_);
    }
    if (total_ == 0) {
      throw ArithmeticError{"frequency model total must be non-zero"};
    }
  }

  std::uint32_t SymbolLimit() const {
    return static_cast<std::uint32_t>(frequencies_.size());
  }

  std::uint32_t Total() const { return total_; }

  std::uint32_t Low(std::uint32_t symbol) const {
    CheckSymbol(symbol);
    return cumulative_[symbol];
  }

  std::uint32_t High(std::uint32_t symbol) const {
    CheckSymbol(symbol);
    return cumulative_[symbol + 1];
  }

  std::uint32_t Frequency(std::uint32_t symbol) const {
    CheckSymbol(symbol);
    return frequencies_[symbol];
  }

  std::uint32_t SymbolForCumulative(std::uint64_t value) const {
    auto it = std::upper_bound(cumulative_.begin(), cumulative_.end(),
                               static_cast<std::uint32_t>(value));
    if (it == cumulative_.begin()) {
      throw ArithmeticError{"bad cumulative value"};
    }
    auto symbol = static_cast<std::uint32_t>(
        std::distance(cumulative_.begin(), it) - 1);
    if (symbol >= frequencies_.size() || Frequency(symbol) == 0) {
      throw ArithmeticError{"decoded zero-frequency symbol"};
    }
    return symbol;
  }

 private:
  void CheckSymbol(std::uint32_t symbol) const {
    if (symbol >= frequencies_.size()) {
      throw ArithmeticError{"symbol out of frequency model range"};
    }
  }

  std::vector<std::uint32_t> frequencies_;
  std::vector<std::uint32_t> cumulative_;
  std::uint32_t total_ = 0;
};

inline FrequencyModel BuildFrequencyModel(std::span<Symbol const> symbols,
                                          std::uint32_t symbol_limit) {
  auto frequencies = std::vector<std::uint32_t>(symbol_limit, 0);
  for (auto symbol : symbols) {
    if (symbol >= symbol_limit) {
      throw ArithmeticError{"symbol exceeds arithmetic alphabet"};
    }
    if (frequencies[symbol] == std::numeric_limits<std::uint32_t>::max()) {
      throw ArithmeticError{"symbol frequency overflow"};
    }
    ++frequencies[symbol];
  }
  return FrequencyModel{std::move(frequencies)};
}

namespace detail {

class CoderBase {
 public:
  explicit CoderBase(int num_state_bits) {
    if (num_state_bits < 1 || num_state_bits > 63) {
      throw ArithmeticError{"state size out of range"};
    }
    num_state_bits_ = num_state_bits;
    full_range_ = std::uint64_t{1} << num_state_bits_;
    half_range_ = full_range_ >> 1;
    quarter_range_ = half_range_ >> 1;
    minimum_range_ = quarter_range_ + 2;
    maximum_total_ =
        std::min(std::numeric_limits<std::uint64_t>::max() / full_range_,
                 minimum_range_);
    state_mask_ = full_range_ - 1;
    low_ = 0;
    high_ = state_mask_;
  }

 protected:
  void Update(FrequencyModel const& frequencies, std::uint32_t symbol) {
    auto const range = high_ - low_ + 1;
    auto const total = frequencies.Total();
    auto const symbol_low = frequencies.Low(symbol);
    auto const symbol_high = frequencies.High(symbol);
    if (symbol_low == symbol_high) {
      throw ArithmeticError{"symbol has zero frequency"};
    }
    if (total > maximum_total_) {
      throw ArithmeticError{"frequency total is too large"};
    }

    auto const new_low = low_ + symbol_low * range / total;
    auto const new_high = low_ + symbol_high * range / total - 1;
    low_ = new_low;
    high_ = new_high;

    while (((low_ ^ high_) & half_range_) == 0) {
      Shift();
      low_ = (low_ << 1) & state_mask_;
      high_ = ((high_ << 1) & state_mask_) | 1;
    }
    while ((low_ & ~high_ & quarter_range_) != 0) {
      Underflow();
      low_ = (low_ << 1) ^ half_range_;
      high_ = ((high_ ^ half_range_) << 1) | half_range_ | 1;
    }
  }

  virtual void Shift() = 0;
  virtual void Underflow() = 0;

  int num_state_bits_ = 0;
  std::uint64_t full_range_ = 0;
  std::uint64_t half_range_ = 0;
  std::uint64_t quarter_range_ = 0;
  std::uint64_t minimum_range_ = 0;
  std::uint64_t maximum_total_ = 0;
  std::uint64_t state_mask_ = 0;
  std::uint64_t low_ = 0;
  std::uint64_t high_ = 0;
};

class Encoder final : public CoderBase {
 public:
  Encoder(int num_state_bits, BitWriter& output)
      : CoderBase{num_state_bits}, output_{output} {}

  void Write(FrequencyModel const& frequencies, std::uint32_t symbol) {
    Update(frequencies, symbol);
  }

  void Finish() { output_.Write(1); }

 private:
  void Shift() override {
    auto const bit = static_cast<int>(low_ >> (num_state_bits_ - 1));
    output_.Write(bit);
    for (; underflow_bits_ > 0; --underflow_bits_) {
      output_.Write(bit ^ 1);
    }
  }

  void Underflow() override {
    if (underflow_bits_ == std::numeric_limits<unsigned long>::max()) {
      throw ArithmeticError{"underflow counter overflow"};
    }
    ++underflow_bits_;
  }

  BitWriter& output_;
  unsigned long underflow_bits_ = 0;
};

class Decoder final : public CoderBase {
 public:
  Decoder(int num_state_bits, BitReader& input)
      : CoderBase{num_state_bits}, input_{input} {
    for (int i = 0; i < num_state_bits_; ++i) {
      code_ = (code_ << 1) | ReadCodeBit();
    }
  }

  std::uint32_t Read(FrequencyModel const& frequencies) {
    auto const total = frequencies.Total();
    if (total > maximum_total_) {
      throw ArithmeticError{"frequency total is too large"};
    }

    auto const range = high_ - low_ + 1;
    auto const offset = code_ - low_;
    auto const value = ((offset + 1) * total - 1) / range;
    if (value >= total) {
      throw ArithmeticError{"decoded cumulative value out of range"};
    }

    auto const symbol = frequencies.SymbolForCumulative(value);
    if (!(frequencies.Low(symbol) * range / total <= offset &&
          offset < frequencies.High(symbol) * range / total)) {
      throw ArithmeticError{"decoded symbol range mismatch"};
    }
    Update(frequencies, symbol);
    if (!(low_ <= code_ && code_ <= high_)) {
      throw ArithmeticError{"decoder code out of range"};
    }
    return symbol;
  }

 private:
  void Shift() override {
    code_ = ((code_ << 1) & state_mask_) | ReadCodeBit();
  }

  void Underflow() override {
    code_ = (code_ & half_range_) | ((code_ << 1) & (state_mask_ >> 1)) |
            ReadCodeBit();
  }

  int ReadCodeBit() {
    auto bit = input_.Read();
    return bit == -1 ? 0 : bit;
  }

  BitReader& input_;
  std::uint64_t code_ = 0;
};

}  // namespace detail

inline std::vector<Byte> Encode(std::span<Symbol const> symbols,
                                FrequencyModel const& frequencies,
                                int state_bits = 32) {
  auto writer = BitWriter{};
  auto encoder = detail::Encoder{state_bits, writer};
  for (auto symbol : symbols) {
    encoder.Write(frequencies, symbol);
  }
  encoder.Finish();
  return writer.Finish();
}

inline std::vector<Symbol> Decode(std::span<Byte const> bytes,
                                  std::size_t symbol_count,
                                  FrequencyModel const& frequencies,
                                  int state_bits = 32) {
  auto reader = BitReader{bytes};
  auto decoder = detail::Decoder{state_bits, reader};
  auto symbols = std::vector<Symbol>{};
  symbols.reserve(symbol_count);
  for (std::size_t i = 0; i < symbol_count; ++i) {
    symbols.push_back(decoder.Read(frequencies));
  }
  return symbols;
}

inline std::vector<Symbol> FlattenModel(Model const& model) {
  auto flattened = std::vector<Symbol>{};
  auto total = model.stream.size();
  for (auto const& rule : model.rules) {
    total += rule.symbols.size();
  }
  flattened.reserve(total);
  flattened.insert(flattened.end(), model.stream.begin(), model.stream.end());
  for (auto const& rule : model.rules) {
    flattened.insert(flattened.end(), rule.symbols.begin(), rule.symbols.end());
  }
  return flattened;
}

inline std::uint32_t AlphabetSize(Model const& model) {
  return static_cast<std::uint32_t>(kFirstRuleSymbol + model.rules.size());
}

inline std::vector<Byte> EncodeModelPayload(Model const& model) {
  auto symbols = FlattenModel(model);
  if (symbols.empty()) {
    return {};
  }
  auto frequencies = BuildFrequencyModel(symbols, AlphabetSize(model));
  return Encode(symbols, frequencies);
}

inline std::vector<Symbol> DecodeModelPayload(std::span<Byte const> bytes,
                                              Model const& model_shape) {
  auto symbols = FlattenModel(model_shape);
  if (symbols.empty()) {
    return {};
  }
  auto frequencies = BuildFrequencyModel(symbols, AlphabetSize(model_shape));
  return Decode(bytes, symbols.size(), frequencies);
}

}  // namespace ae::compression::arithmetic

#endif  // AE_COMPRESSION_ARITHMETIC_HPP_
