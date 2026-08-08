/*
 * Copyright 2026 Aethernet Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#ifndef AE_COMPRESSION_EXPERIMENTAL_PRIOR_RESIDUAL_ARITHMETIC_HPP_
#define AE_COMPRESSION_EXPERIMENTAL_PRIOR_RESIDUAL_ARITHMETIC_HPP_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "ae_compression/experimental/context_arithmetic.hpp"

namespace ae::compression::experimental {

enum class PriorResidualMode : std::uint8_t {
  // Use one global table, masked by the symbols structurally available at each
  // rule height. No per-height metadata is transmitted.
  kPriorOnly = 0,

  // Predict each height's exact counts from the global table, then transmit
  // only sparse signed residuals. Arithmetic coding uses the reconstructed
  // exact per-height counts.
  kExactResidual = 1,
};

inline char const* PriorResidualModeName(PriorResidualMode mode) {
  switch (mode) {
    case PriorResidualMode::kPriorOnly:
      return "height_global_prior";
    case PriorResidualMode::kExactResidual:
      return "height_global_prior_exact_residual";
  }
  return "unknown";
}

struct PriorResidualStats {
  std::size_t context_count = 0;
  std::size_t level_count = 0;
  std::size_t global_frequency_entries = 0;
  std::size_t actual_frequency_entries = 0;
  std::size_t model_frequency_entries = 0;
  std::size_t residual_entries = 0;
  std::size_t largest_context_alphabet = 0;
  double ideal_payload_bits = 0.0;
  std::size_t payload_bytes = 0;
  std::size_t fixed_header_bytes = 0;
  std::size_t shape_bytes = 0;
  std::size_t prior_bytes = 0;
  std::size_t residual_bytes = 0;
  std::size_t payload_size_bytes = 0;
  std::size_t frame_bytes = 0;
};

struct PriorResidualFrame {
  std::vector<Byte> bytes;
  PriorResidualStats stats;
};

namespace prior_detail {

inline constexpr std::array<Byte, 4> kMagic = {'A', 'E', 'R', '1'};

inline std::uint64_t ZigZagEncode(std::int64_t value) {
  if (value >= 0) {
    return static_cast<std::uint64_t>(value) * 2;
  }
  return static_cast<std::uint64_t>(-(value + 1)) * 2 + 1;
}

inline std::int64_t ZigZagDecode(std::uint64_t value) {
  if ((value & 1U) == 0) {
    if (value / 2 >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      throw FormatError{"prior residual is too large"};
    }
    return static_cast<std::int64_t>(value / 2);
  }
  auto const magnitude = value / 2;
  if (magnitude >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw FormatError{"negative prior residual is too large"};
  }
  return -static_cast<std::int64_t>(magnitude) - 1;
}

inline std::size_t AllowedSymbolLimit(
    std::size_t rule_count,
    std::vector<std::size_t> const& group_rule_counts,
    std::size_t context) {
  auto const alphabet = static_cast<std::size_t>(kFirstRuleSymbol) + rule_count;
  if (context == 0) {
    return alphabet;
  }
  auto const level = context - 1;
  if (level >= group_rule_counts.size()) {
    throw FormatError{"prior context level is out of range"};
  }
  auto lower_rules = std::size_t{0};
  for (std::size_t i = 0; i < level; ++i) {
    if (group_rule_counts[i] >
        std::numeric_limits<std::size_t>::max() - lower_rules) {
      throw FormatError{"prior lower-rule count overflow"};
    }
    lower_rules += group_rule_counts[i];
  }
  if (lower_rules > rule_count) {
    throw FormatError{"prior lower-rule count exceeds rule count"};
  }
  return static_cast<std::size_t>(kFirstRuleSymbol) + lower_rules;
}

inline std::vector<std::uint32_t> SumFrequencies(
    std::vector<std::vector<std::uint32_t>> const& frequencies) {
  if (frequencies.empty()) {
    return {};
  }
  auto result = std::vector<std::uint32_t>(frequencies.front().size(), 0);
  for (auto const& table : frequencies) {
    if (table.size() != result.size()) {
      throw FormatError{"prior frequency tables have different alphabets"};
    }
    for (std::size_t symbol = 0; symbol < table.size(); ++symbol) {
      if (table[symbol] >
          std::numeric_limits<std::uint32_t>::max() - result[symbol]) {
        throw FormatError{"global prior frequency overflow"};
      }
      result[symbol] += table[symbol];
    }
  }
  return result;
}

inline std::uint64_t SumTable(std::span<std::uint32_t const> table) {
  auto total = std::uint64_t{0};
  for (auto frequency : table) {
    total += frequency;
  }
  return total;
}

inline std::vector<std::vector<std::uint32_t>> BuildMaskedPrior(
    std::vector<std::uint32_t> const& global,
    std::vector<std::size_t> const& group_rule_counts,
    std::vector<std::uint64_t> const& context_totals) {
  auto const rule_count =
      global.size() < kFirstRuleSymbol ? 0 : global.size() - kFirstRuleSymbol;
  if (context_totals.size() != group_rule_counts.size() + 1) {
    throw FormatError{"prior context total count mismatch"};
  }

  auto result = std::vector<std::vector<std::uint32_t>>(
      context_totals.size(), std::vector<std::uint32_t>(global.size(), 0));
  for (std::size_t context = 0; context < result.size(); ++context) {
    auto const limit =
        AllowedSymbolLimit(rule_count, group_rule_counts, context);
    for (std::size_t symbol = 0; symbol < limit; ++symbol) {
      result[context][symbol] = global[symbol];
    }
    if (context_totals[context] != 0 && SumTable(result[context]) == 0) {
      throw FormatError{"non-empty prior context has an empty model"};
    }
  }
  return result;
}

struct RemainderEntry {
  std::uint64_t remainder = 0;
  std::uint32_t global_frequency = 0;
  std::size_t symbol = 0;
};

inline std::vector<std::uint32_t> PredictTable(
    std::vector<std::uint32_t> const& global, std::size_t allowed_limit,
    std::uint64_t context_total) {
  auto predicted = std::vector<std::uint32_t>(global.size(), 0);
  if (context_total == 0) {
    return predicted;
  }
  if (allowed_limit > global.size()) {
    throw FormatError{"prior allowed alphabet exceeds global alphabet"};
  }

  auto denominator = std::uint64_t{0};
  for (std::size_t symbol = 0; symbol < allowed_limit; ++symbol) {
    denominator += global[symbol];
  }
  if (denominator == 0) {
    throw FormatError{"cannot predict a non-empty context from an empty prior"};
  }

  auto remainders = std::vector<RemainderEntry>{};
  remainders.reserve(allowed_limit);
  auto assigned = std::uint64_t{0};
  for (std::size_t symbol = 0; symbol < allowed_limit; ++symbol) {
    auto const weight = global[symbol];
    if (weight == 0) {
      continue;
    }
    if (context_total >
        std::numeric_limits<std::uint64_t>::max() / weight) {
      throw FormatError{"prior prediction multiplication overflow"};
    }
    auto const product = context_total * weight;
    auto const base = product / denominator;
    if (base > std::numeric_limits<std::uint32_t>::max()) {
      throw FormatError{"predicted frequency exceeds uint32"};
    }
    predicted[symbol] = static_cast<std::uint32_t>(base);
    assigned += base;
    remainders.push_back(
        RemainderEntry{product % denominator, weight, symbol});
  }
  if (assigned > context_total) {
    throw FormatError{"prior prediction assigned too many symbols"};
  }

  auto remaining = context_total - assigned;
  std::sort(remainders.begin(), remainders.end(),
            [](RemainderEntry const& left, RemainderEntry const& right) {
              if (left.remainder != right.remainder) {
                return left.remainder > right.remainder;
              }
              if (left.global_frequency != right.global_frequency) {
                return left.global_frequency > right.global_frequency;
              }
              return left.symbol < right.symbol;
            });
  if (remaining > remainders.size()) {
    throw FormatError{"prior rounding remainder exceeds alphabet"};
  }
  for (std::size_t i = 0; i < static_cast<std::size_t>(remaining); ++i) {
    auto& frequency = predicted[remainders[i].symbol];
    if (frequency == std::numeric_limits<std::uint32_t>::max()) {
      throw FormatError{"predicted frequency increment overflow"};
    }
    ++frequency;
  }
  return predicted;
}

inline std::vector<std::vector<std::uint32_t>> BuildPredictions(
    std::vector<std::uint32_t> const& global,
    std::vector<std::size_t> const& group_rule_counts,
    std::vector<std::uint64_t> const& context_totals) {
  auto const rule_count =
      global.size() < kFirstRuleSymbol ? 0 : global.size() - kFirstRuleSymbol;
  if (context_totals.size() != group_rule_counts.size() + 1) {
    throw FormatError{"prior prediction context count mismatch"};
  }
  auto result = std::vector<std::vector<std::uint32_t>>{};
  result.reserve(context_totals.size());
  for (std::size_t context = 0; context < context_totals.size(); ++context) {
    result.push_back(PredictTable(
        global, AllowedSymbolLimit(rule_count, group_rule_counts, context),
        context_totals[context]));
  }
  return result;
}

inline std::vector<std::pair<std::size_t, std::int64_t>> Residuals(
    std::span<std::uint32_t const> actual,
    std::span<std::uint32_t const> predicted) {
  if (actual.size() != predicted.size()) {
    throw FormatError{"prior residual alphabet mismatch"};
  }
  auto result = std::vector<std::pair<std::size_t, std::int64_t>>{};
  auto sum = std::int64_t{0};
  for (std::size_t symbol = 0; symbol < actual.size(); ++symbol) {
    auto const residual = static_cast<std::int64_t>(actual[symbol]) -
                          static_cast<std::int64_t>(predicted[symbol]);
    if (residual != 0) {
      result.push_back({symbol, residual});
      if ((residual > 0 &&
           sum > std::numeric_limits<std::int64_t>::max() - residual) ||
          (residual < 0 &&
           sum < std::numeric_limits<std::int64_t>::min() - residual)) {
        throw FormatError{"prior residual sum overflow"};
      }
      sum += residual;
    }
  }
  if (sum != 0) {
    throw FormatError{"prior residuals do not sum to zero"};
  }
  if (result.size() == 1) {
    throw FormatError{"a single non-zero residual cannot sum to zero"};
  }
  return result;
}

inline void WriteSparsePrior(std::vector<Byte>& out,
                             std::vector<std::uint32_t> const& global) {
  auto count = std::size_t{0};
  for (auto frequency : global) {
    if (frequency != 0) {
      ++count;
    }
  }
  ae::compression::detail::WriteUvarint(out, count);
  auto next_symbol = std::size_t{0};
  for (std::size_t symbol = 0; symbol < global.size(); ++symbol) {
    if (global[symbol] == 0) {
      continue;
    }
    ae::compression::detail::WriteUvarint(out, symbol - next_symbol);
    ae::compression::detail::WriteUvarint(out, global[symbol]);
    next_symbol = symbol + 1;
  }
}

inline std::vector<std::uint32_t> ReadSparsePrior(
    std::span<Byte const> data, std::size_t& pos, std::size_t alphabet_size) {
  auto result = std::vector<std::uint32_t>(alphabet_size, 0);
  auto const count = context_detail::CheckedSize(
      ae::compression::detail::ReadUvarint(data, pos));
  auto next_symbol = std::size_t{0};
  for (std::size_t i = 0; i < count; ++i) {
    auto const delta = context_detail::CheckedSize(
        ae::compression::detail::ReadUvarint(data, pos));
    if (delta > alphabet_size - std::min(next_symbol, alphabet_size)) {
      throw FormatError{"prior symbol delta exceeds alphabet"};
    }
    auto const symbol = next_symbol + delta;
    if (symbol >= alphabet_size) {
      throw FormatError{"prior symbol is out of range"};
    }
    auto const frequency =
        ae::compression::detail::ReadUvarint(data, pos);
    if (frequency == 0 ||
        frequency > std::numeric_limits<std::uint32_t>::max()) {
      throw FormatError{"bad global prior frequency"};
    }
    result[symbol] = static_cast<std::uint32_t>(frequency);
    next_symbol = symbol + 1;
  }
  return result;
}

inline std::size_t WriteResidualTables(
    std::vector<Byte>& out,
    std::vector<std::vector<std::uint32_t>> const& actual,
    std::vector<std::vector<std::uint32_t>> const& predicted) {
  if (actual.size() != predicted.size()) {
    throw FormatError{"prior residual table count mismatch"};
  }
  auto total_entries = std::size_t{0};
  for (std::size_t context = 0; context < actual.size(); ++context) {
    auto entries = Residuals(actual[context], predicted[context]);
    total_entries += entries.size();
    ae::compression::detail::WriteUvarint(out, entries.size());
    auto next_symbol = std::size_t{0};
    auto stored_sum = std::int64_t{0};
    for (std::size_t i = 0; i < entries.size(); ++i) {
      auto const [symbol, residual] = entries[i];
      ae::compression::detail::WriteUvarint(out, symbol - next_symbol);
      next_symbol = symbol + 1;
      if (i + 1 != entries.size()) {
        ae::compression::detail::WriteUvarint(out,
                                               ZigZagEncode(residual));
        stored_sum += residual;
      } else if (residual != -stored_sum) {
        throw FormatError{"omitted prior residual cannot be reconstructed"};
      }
    }
  }
  return total_entries;
}

inline std::vector<std::vector<std::uint32_t>> ReadResidualTables(
    std::span<Byte const> data, std::size_t& pos,
    std::vector<std::vector<std::uint32_t>> predicted,
    std::vector<std::uint64_t> const& context_totals) {
  if (predicted.size() != context_totals.size()) {
    throw FormatError{"prior residual context total mismatch"};
  }
  for (std::size_t context = 0; context < predicted.size(); ++context) {
    auto const count = context_detail::CheckedSize(
        ae::compression::detail::ReadUvarint(data, pos));
    if (count == 1) {
      throw FormatError{"prior residual table cannot contain one entry"};
    }
    auto next_symbol = std::size_t{0};
    auto stored_sum = std::int64_t{0};
    for (std::size_t i = 0; i < count; ++i) {
      auto const delta = context_detail::CheckedSize(
          ae::compression::detail::ReadUvarint(data, pos));
      if (delta >
          predicted[context].size() -
              std::min(next_symbol, predicted[context].size())) {
        throw FormatError{"prior residual symbol delta exceeds alphabet"};
      }
      auto const symbol = next_symbol + delta;
      if (symbol >= predicted[context].size()) {
        throw FormatError{"prior residual symbol is out of range"};
      }
      auto residual = std::int64_t{0};
      if (i + 1 != count) {
        residual = ZigZagDecode(
            ae::compression::detail::ReadUvarint(data, pos));
        stored_sum += residual;
      } else {
        residual = -stored_sum;
      }
      auto const updated =
          static_cast<std::int64_t>(predicted[context][symbol]) + residual;
      if (updated < 0 ||
          static_cast<std::uint64_t>(updated) >
              std::numeric_limits<std::uint32_t>::max()) {
        throw FormatError{"prior residual produces an invalid frequency"};
      }
      predicted[context][symbol] = static_cast<std::uint32_t>(updated);
      next_symbol = symbol + 1;
    }
    if (SumTable(predicted[context]) != context_totals[context]) {
      throw FormatError{"reconstructed prior table has the wrong total"};
    }
  }
  return predicted;
}

inline double CrossEntropyBits(
    std::vector<std::vector<std::uint32_t>> const& actual,
    std::vector<std::vector<std::uint32_t>> const& models) {
  if (actual.size() != models.size()) {
    throw FormatError{"prior cross-entropy context count mismatch"};
  }
  auto bits = 0.0;
  for (std::size_t context = 0; context < actual.size(); ++context) {
    auto const model_total = SumTable(models[context]);
    auto const actual_total = SumTable(actual[context]);
    if (actual_total == 0) {
      continue;
    }
    if (model_total == 0) {
      throw FormatError{"non-empty prior context has an empty coding model"};
    }
    for (std::size_t symbol = 0; symbol < actual[context].size(); ++symbol) {
      auto const count = actual[context][symbol];
      if (count == 0) {
        continue;
      }
      auto const weight = models[context][symbol];
      if (weight == 0) {
        throw FormatError{"prior coding model assigns zero probability"};
      }
      bits -= static_cast<double>(count) *
              std::log2(static_cast<double>(weight) /
                        static_cast<double>(model_total));
    }
  }
  return bits;
}

inline std::size_t CountNonZero(
    std::vector<std::vector<std::uint32_t>> const& frequencies) {
  auto count = std::size_t{0};
  for (auto const& table : frequencies) {
    for (auto frequency : table) {
      if (frequency != 0) {
        ++count;
      }
    }
  }
  return count;
}

inline std::size_t LargestAlphabet(
    std::vector<std::vector<std::uint32_t>> const& frequencies) {
  auto largest = std::size_t{0};
  for (auto const& table : frequencies) {
    auto count = std::size_t{0};
    for (auto frequency : table) {
      if (frequency != 0) {
        ++count;
      }
    }
    largest = std::max(largest, count);
  }
  return largest;
}

inline std::vector<std::uint32_t> CountDecodedGlobal(Model const& model) {
  auto counts = std::vector<std::uint32_t>(
      static_cast<std::size_t>(kFirstRuleSymbol) + model.rules.size(), 0);
  auto count_layer = [&](std::vector<Symbol> const& layer) {
    for (auto symbol : layer) {
      if (symbol >= counts.size()) {
        throw FormatError{"decoded prior symbol exceeds alphabet"};
      }
      if (counts[symbol] == std::numeric_limits<std::uint32_t>::max()) {
        throw FormatError{"decoded prior frequency overflow"};
      }
      ++counts[symbol];
    }
  };
  count_layer(model.stream);
  for (auto const& rule : model.rules) {
    count_layer(rule.symbols);
  }
  return counts;
}

}  // namespace prior_detail

inline PriorResidualFrame PackPriorResidual(Model const& source,
                                            PriorResidualMode mode) {
  auto plan = context_detail::BuildPlan(source, ContextScheme::kRuleHeight);
  auto actual = context_detail::BuildFrequencies(plan);
  auto global = prior_detail::SumFrequencies(actual);

  auto rule_lengths = std::vector<std::size_t>{};
  rule_lengths.reserve(plan.model.rules.size());
  for (auto const& rule : plan.model.rules) {
    rule_lengths.push_back(rule.symbols.size());
  }
  auto context_totals = context_detail::ExpectedContextTotals(
      plan.model.stream.size(), rule_lengths, plan.layer_contexts,
      plan.context_count);

  auto model_frequencies =
      prior_detail::BuildMaskedPrior(global, plan.group_rule_counts,
                                     context_totals);
  auto predictions = std::vector<std::vector<std::uint32_t>>{};
  if (mode == PriorResidualMode::kExactResidual) {
    predictions = prior_detail::BuildPredictions(
        global, plan.group_rule_counts, context_totals);
    model_frequencies = actual;
  }
  auto payload = context_detail::EncodePayload(plan, model_frequencies);

  auto out = std::vector<Byte>{};
  out.insert(out.end(), prior_detail::kMagic.begin(),
             prior_detail::kMagic.end());
  out.push_back(static_cast<Byte>(mode));
  auto const fixed_header_bytes = out.size();

  auto const shape_begin = out.size();
  ae::compression::detail::WriteUvarint(out, plan.model.rules.size());
  ae::compression::detail::WriteUvarint(out, plan.model.stream.size());
  for (auto const& rule : plan.model.rules) {
    ae::compression::detail::WriteUvarint(out, rule.symbols.size());
  }
  ae::compression::detail::WriteUvarint(out,
                                         plan.group_rule_counts.size());
  for (auto count : plan.group_rule_counts) {
    ae::compression::detail::WriteUvarint(out, count);
  }
  auto const shape_bytes = out.size() - shape_begin;

  auto const prior_begin = out.size();
  prior_detail::WriteSparsePrior(out, global);
  auto const prior_bytes = out.size() - prior_begin;

  auto residual_entries = std::size_t{0};
  auto const residual_begin = out.size();
  if (mode == PriorResidualMode::kExactResidual) {
    residual_entries =
        prior_detail::WriteResidualTables(out, actual, predictions);
  }
  auto const residual_bytes = out.size() - residual_begin;

  auto const payload_size_begin = out.size();
  ae::compression::detail::WriteUvarint(out, payload.size());
  auto const payload_size_bytes = out.size() - payload_size_begin;
  out.insert(out.end(), payload.begin(), payload.end());

  auto global_entries = std::size_t{0};
  for (auto frequency : global) {
    if (frequency != 0) {
      ++global_entries;
    }
  }
  auto stats = PriorResidualStats{};
  stats.context_count = plan.context_count;
  stats.level_count = plan.group_rule_counts.size();
  stats.global_frequency_entries = global_entries;
  stats.actual_frequency_entries = prior_detail::CountNonZero(actual);
  stats.model_frequency_entries =
      prior_detail::CountNonZero(model_frequencies);
  stats.residual_entries = residual_entries;
  stats.largest_context_alphabet =
      prior_detail::LargestAlphabet(model_frequencies);
  stats.ideal_payload_bits =
      prior_detail::CrossEntropyBits(actual, model_frequencies);
  stats.payload_bytes = payload.size();
  stats.fixed_header_bytes = fixed_header_bytes;
  stats.shape_bytes = shape_bytes;
  stats.prior_bytes = prior_bytes;
  stats.residual_bytes = residual_bytes;
  stats.payload_size_bytes = payload_size_bytes;
  stats.frame_bytes = out.size();
  return PriorResidualFrame{std::move(out), stats};
}

inline Model UnpackPriorResidual(std::span<Byte const> data) {
  if (data.size() < prior_detail::kMagic.size() + 1) {
    throw FormatError{"prior residual frame is too small"};
  }
  auto pos = std::size_t{0};
  for (auto expected : prior_detail::kMagic) {
    if (data[pos++] != expected) {
      throw FormatError{"bad prior residual frame magic"};
    }
  }
  auto const raw_mode = data[pos++];
  if (raw_mode > static_cast<Byte>(PriorResidualMode::kExactResidual)) {
    throw FormatError{"unknown prior residual mode"};
  }
  auto const mode = static_cast<PriorResidualMode>(raw_mode);

  auto const rule_count = context_detail::CheckedSize(
      ae::compression::detail::ReadUvarint(data, pos));
  auto const stream_size = context_detail::CheckedSize(
      ae::compression::detail::ReadUvarint(data, pos));
  auto rule_lengths = std::vector<std::size_t>{};
  rule_lengths.reserve(rule_count);
  for (std::size_t i = 0; i < rule_count; ++i) {
    rule_lengths.push_back(context_detail::CheckedSize(
        ae::compression::detail::ReadUvarint(data, pos)));
  }

  auto const level_count = context_detail::CheckedSize(
      ae::compression::detail::ReadUvarint(data, pos));
  auto group_rule_counts = std::vector<std::size_t>{};
  group_rule_counts.reserve(level_count);
  auto covered_rules = std::size_t{0};
  for (std::size_t level = 0; level < level_count; ++level) {
    auto const count = context_detail::CheckedSize(
        ae::compression::detail::ReadUvarint(data, pos));
    if (count > rule_count - covered_rules) {
      throw FormatError{"prior level rule count overflow"};
    }
    group_rule_counts.push_back(count);
    covered_rules += count;
  }
  if (covered_rules != rule_count) {
    throw FormatError{"prior levels do not cover all rules"};
  }

  auto layer_contexts = std::vector<std::size_t>(rule_count + 1, 0);
  auto rule_pos = std::size_t{0};
  for (std::size_t level = 0; level < level_count; ++level) {
    for (std::size_t i = 0; i < group_rule_counts[level]; ++i) {
      layer_contexts[rule_pos + i + 1] = level + 1;
    }
    rule_pos += group_rule_counts[level];
  }
  auto const context_count = level_count + 1;
  auto const alphabet_size =
      static_cast<std::size_t>(kFirstRuleSymbol) + rule_count;
  auto global = prior_detail::ReadSparsePrior(data, pos, alphabet_size);

  auto context_totals = context_detail::ExpectedContextTotals(
      stream_size, rule_lengths, layer_contexts, context_count);
  auto total_symbols = std::uint64_t{0};
  for (auto total : context_totals) {
    total_symbols += total;
  }
  if (prior_detail::SumTable(global) != total_symbols) {
    throw FormatError{"global prior total does not match model shape"};
  }

  auto frequencies = prior_detail::BuildMaskedPrior(
      global, group_rule_counts, context_totals);
  if (mode == PriorResidualMode::kExactResidual) {
    auto predictions = prior_detail::BuildPredictions(
        global, group_rule_counts, context_totals);
    frequencies = prior_detail::ReadResidualTables(
        data, pos, std::move(predictions), context_totals);
    if (prior_detail::SumFrequencies(frequencies) != global) {
      throw FormatError{"residual tables do not reproduce the global prior"};
    }
  }

  auto const payload_size = context_detail::CheckedSize(
      ae::compression::detail::ReadUvarint(data, pos));
  if (payload_size != data.size() - pos) {
    throw FormatError{"prior residual payload size mismatch"};
  }
  auto model = context_detail::DecodePayload(
      data.subspan(pos, payload_size), stream_size, rule_lengths,
      layer_contexts, frequencies);
  if (prior_detail::CountDecodedGlobal(model) != global) {
    throw FormatError{"decoded model does not match the global prior"};
  }
  return model;
}

}  // namespace ae::compression::experimental

#endif  // AE_COMPRESSION_EXPERIMENTAL_PRIOR_RESIDUAL_ARITHMETIC_HPP_
