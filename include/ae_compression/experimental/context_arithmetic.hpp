/*
 * Copyright 2026 Aethernet Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#ifndef AE_COMPRESSION_EXPERIMENTAL_CONTEXT_ARITHMETIC_HPP_
#define AE_COMPRESSION_EXPERIMENTAL_CONTEXT_ARITHMETIC_HPP_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ae_compression/format.hpp"

namespace ae::compression::experimental {

enum class ContextScheme : std::uint8_t {
  kGlobal = 0,
  kRuleHeight = 1,
  kMinimumDepth = 2,
  kParentRule = 3,
};

inline char const* ContextSchemeName(ContextScheme scheme) {
  switch (scheme) {
    case ContextScheme::kGlobal:
      return "global";
    case ContextScheme::kRuleHeight:
      return "rule_height";
    case ContextScheme::kMinimumDepth:
      return "minimum_depth";
    case ContextScheme::kParentRule:
      return "parent_rule";
  }
  return "unknown";
}

struct ContextCodingStats {
  std::size_t context_count = 0;
  std::size_t level_count = 0;
  std::size_t non_zero_frequency_entries = 0;
  std::size_t largest_context_alphabet = 0;
  double ideal_payload_bits = 0.0;
  std::size_t payload_bytes = 0;
  std::size_t fixed_header_bytes = 0;
  std::size_t shape_bytes = 0;
  std::size_t frequency_bytes = 0;
  std::size_t payload_size_bytes = 0;
  std::size_t frame_bytes = 0;
};

struct ContextFrame {
  std::vector<Byte> bytes;
  ContextCodingStats stats;
};

namespace context_detail {

inline constexpr std::array<Byte, 4> kMagic = {'A', 'E', 'C', 'X'};

struct ContextPlan {
  Model model;
  std::vector<std::size_t> layer_contexts;
  std::vector<std::size_t> group_rule_counts;
  std::size_t context_count = 0;
};

inline bool IsGrouped(ContextScheme scheme) {
  return scheme == ContextScheme::kRuleHeight ||
         scheme == ContextScheme::kMinimumDepth;
}

inline std::size_t CheckedSize(std::uint64_t value) {
  if (value > std::numeric_limits<std::size_t>::max()) {
    throw FormatError{"context frame size does not fit size_t"};
  }
  return static_cast<std::size_t>(value);
}

inline std::size_t RuleIndex(Model const& model, Symbol symbol) {
  if (symbol < kFirstRuleSymbol) {
    throw FormatError{"literal is not a rule reference"};
  }
  auto const index = static_cast<std::size_t>(symbol - kFirstRuleSymbol);
  if (index >= model.rules.size()) {
    throw FormatError{"context model references an unknown rule"};
  }
  return index;
}

inline std::vector<std::size_t> ComputeRuleHeights(Model const& model) {
  auto heights = std::vector<std::size_t>(model.rules.size(), 0);
  auto states = std::vector<std::uint8_t>(model.rules.size(), 0);

  auto visit = std::function<std::size_t(std::size_t)>{};
  visit = [&](std::size_t index) -> std::size_t {
    if (states[index] == 2) {
      return heights[index];
    }
    if (states[index] == 1) {
      throw FormatError{"context model contains a cyclic rule graph"};
    }

    states[index] = 1;
    auto height = std::size_t{0};
    for (auto symbol : model.rules[index].symbols) {
      if (symbol < kFirstRuleSymbol) {
        continue;
      }
      auto const child = RuleIndex(model, symbol);
      auto const child_height = visit(child);
      if (child_height == std::numeric_limits<std::size_t>::max()) {
        throw FormatError{"context model depth overflow"};
      }
      height = std::max(height, child_height + 1);
    }
    heights[index] = height;
    states[index] = 2;
    return height;
  };

  for (std::size_t i = 0; i < model.rules.size(); ++i) {
    visit(i);
  }
  return heights;
}

inline std::vector<std::size_t> ComputeMinimumDepths(Model const& model) {
  // Validate the graph first. Minimum distance alone would not detect a cycle.
  (void)ComputeRuleHeights(model);

  auto const unreachable = std::numeric_limits<std::size_t>::max();
  auto depths = std::vector<std::size_t>(model.rules.size(), unreachable);
  auto queue = std::vector<std::size_t>{};
  queue.reserve(model.rules.size());

  for (auto symbol : model.stream) {
    if (symbol < kFirstRuleSymbol) {
      continue;
    }
    auto const index = RuleIndex(model, symbol);
    if (depths[index] == unreachable) {
      depths[index] = 0;
      queue.push_back(index);
    }
  }

  auto queue_pos = std::size_t{0};
  while (queue_pos < queue.size()) {
    auto const parent = queue[queue_pos++];
    if (depths[parent] == std::numeric_limits<std::size_t>::max()) {
      throw FormatError{"context model depth overflow"};
    }
    auto const child_depth = depths[parent] + 1;
    for (auto symbol : model.rules[parent].symbols) {
      if (symbol < kFirstRuleSymbol) {
        continue;
      }
      auto const child = RuleIndex(model, symbol);
      if (child_depth < depths[child]) {
        depths[child] = child_depth;
        queue.push_back(child);
      }
    }
  }

  auto max_reachable = std::size_t{0};
  auto has_reachable = false;
  for (auto depth : depths) {
    if (depth != unreachable) {
      max_reachable = std::max(max_reachable, depth);
      has_reachable = true;
    }
  }
  auto const orphan_depth = has_reachable ? max_reachable + 1 : 0;
  for (auto& depth : depths) {
    if (depth == unreachable) {
      depth = orphan_depth;
    }
  }
  return depths;
}

inline Model ReindexByLevel(Model const& model,
                            std::vector<std::size_t> const& levels,
                            std::vector<std::size_t>& group_rule_counts,
                            std::vector<std::size_t>& rule_contexts) {
  if (levels.size() != model.rules.size()) {
    throw FormatError{"context level count does not match rule count"};
  }

  auto order = std::vector<std::size_t>(model.rules.size(), 0);
  for (std::size_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  std::stable_sort(order.begin(), order.end(), [&](std::size_t left,
                                                    std::size_t right) {
    if (levels[left] != levels[right]) {
      return levels[left] < levels[right];
    }
    return left < right;
  });

  auto max_level = std::size_t{0};
  if (!levels.empty()) {
    max_level = *std::max_element(levels.begin(), levels.end());
    group_rule_counts.assign(max_level + 1, 0);
  }
  for (auto level : levels) {
    ++group_rule_counts[level];
  }

  auto old_to_new = std::vector<std::size_t>(model.rules.size(), 0);
  for (std::size_t new_index = 0; new_index < order.size(); ++new_index) {
    old_to_new[order[new_index]] = new_index;
  }

  auto map_symbol = [&](Symbol symbol) -> Symbol {
    if (symbol < kFirstRuleSymbol) {
      return symbol;
    }
    auto const old_index = RuleIndex(model, symbol);
    auto const new_index = old_to_new[old_index];
    if (new_index >
        std::numeric_limits<Symbol>::max() - kFirstRuleSymbol) {
      throw FormatError{"too many rules for context symbol type"};
    }
    return kFirstRuleSymbol + static_cast<Symbol>(new_index);
  };

  auto result = Model{};
  result.stream.reserve(model.stream.size());
  for (auto symbol : model.stream) {
    result.stream.push_back(map_symbol(symbol));
  }

  result.rules.resize(model.rules.size());
  rule_contexts.resize(model.rules.size());
  for (std::size_t new_index = 0; new_index < order.size(); ++new_index) {
    auto const old_index = order[new_index];
    auto& output = result.rules[new_index].symbols;
    output.reserve(model.rules[old_index].symbols.size());
    for (auto symbol : model.rules[old_index].symbols) {
      output.push_back(map_symbol(symbol));
    }
    rule_contexts[new_index] = levels[old_index] + 1;
  }
  return result;
}

inline ContextPlan BuildPlan(Model const& source, ContextScheme scheme) {
  auto plan = ContextPlan{};

  if (scheme == ContextScheme::kGlobal) {
    plan.model = source;
    plan.layer_contexts.assign(source.rules.size() + 1, 0);
    plan.context_count = 1;
    return plan;
  }

  if (scheme == ContextScheme::kParentRule) {
    plan.model = source;
    plan.layer_contexts.resize(source.rules.size() + 1);
    for (std::size_t i = 0; i < plan.layer_contexts.size(); ++i) {
      plan.layer_contexts[i] = i;
    }
    plan.context_count = plan.layer_contexts.size();
    return plan;
  }

  auto levels = scheme == ContextScheme::kRuleHeight
                    ? ComputeRuleHeights(source)
                    : ComputeMinimumDepths(source);
  auto rule_contexts = std::vector<std::size_t>{};
  plan.model = ReindexByLevel(source, levels, plan.group_rule_counts,
                              rule_contexts);
  plan.layer_contexts.resize(plan.model.rules.size() + 1);
  plan.layer_contexts[0] = 0;
  for (std::size_t i = 0; i < rule_contexts.size(); ++i) {
    plan.layer_contexts[i + 1] = rule_contexts[i];
  }
  plan.context_count = plan.group_rule_counts.size() + 1;
  return plan;
}

inline std::size_t AlphabetSize(Model const& model) {
  auto const rules = model.rules.size();
  if (rules > std::numeric_limits<Symbol>::max() - kFirstRuleSymbol) {
    throw FormatError{"too many rules for context alphabet"};
  }
  return static_cast<std::size_t>(kFirstRuleSymbol) + rules;
}

inline std::vector<std::vector<std::uint32_t>> BuildFrequencies(
    ContextPlan const& plan) {
  auto frequencies = std::vector<std::vector<std::uint32_t>>(
      plan.context_count,
      std::vector<std::uint32_t>(AlphabetSize(plan.model), 0));

  auto count_layer = [&](std::vector<Symbol> const& layer,
                         std::size_t context) {
    if (context >= frequencies.size()) {
      throw FormatError{"context index is out of range"};
    }
    for (auto symbol : layer) {
      if (symbol >= frequencies[context].size()) {
        throw FormatError{"context symbol exceeds alphabet"};
      }
      auto& frequency = frequencies[context][symbol];
      if (frequency == std::numeric_limits<std::uint32_t>::max()) {
        throw FormatError{"context frequency overflow"};
      }
      ++frequency;
    }
  };

  count_layer(plan.model.stream, plan.layer_contexts[0]);
  for (std::size_t i = 0; i < plan.model.rules.size(); ++i) {
    count_layer(plan.model.rules[i].symbols, plan.layer_contexts[i + 1]);
  }
  return frequencies;
}

inline std::vector<std::optional<arithmetic::FrequencyModel>> BuildModels(
    std::vector<std::vector<std::uint32_t>> const& frequencies) {
  auto models =
      std::vector<std::optional<arithmetic::FrequencyModel>>{};
  models.reserve(frequencies.size());
  for (auto const& table : frequencies) {
    auto total = std::uint64_t{0};
    for (auto frequency : table) {
      total += frequency;
    }
    if (total == 0) {
      models.push_back(std::nullopt);
    } else {
      models.emplace_back(std::in_place, table);
    }
  }
  return models;
}

inline std::size_t TotalSymbols(Model const& model) {
  auto total = model.stream.size();
  for (auto const& rule : model.rules) {
    if (rule.symbols.size() > std::numeric_limits<std::size_t>::max() - total) {
      throw FormatError{"context symbol count overflow"};
    }
    total += rule.symbols.size();
  }
  return total;
}

inline std::vector<Byte> EncodePayload(
    ContextPlan const& plan,
    std::vector<std::vector<std::uint32_t>> const& frequencies) {
  if (TotalSymbols(plan.model) == 0) {
    return {};
  }

  auto models = BuildModels(frequencies);
  auto writer = arithmetic::BitWriter{};
  auto encoder = arithmetic::detail::Encoder{32, writer};

  auto write_layer = [&](std::vector<Symbol> const& layer,
                         std::size_t context) {
    if (layer.empty()) {
      return;
    }
    if (context >= models.size() || !models[context].has_value()) {
      throw FormatError{"missing arithmetic model for context"};
    }
    for (auto symbol : layer) {
      encoder.Write(*models[context], symbol);
    }
  };

  write_layer(plan.model.stream, plan.layer_contexts[0]);
  for (std::size_t i = 0; i < plan.model.rules.size(); ++i) {
    write_layer(plan.model.rules[i].symbols, plan.layer_contexts[i + 1]);
  }
  encoder.Finish();
  return writer.Finish();
}

inline Model DecodePayload(
    std::span<Byte const> payload, std::size_t stream_size,
    std::vector<std::size_t> const& rule_lengths,
    std::vector<std::size_t> const& layer_contexts,
    std::vector<std::vector<std::uint32_t>> const& frequencies) {
  auto total = stream_size;
  for (auto length : rule_lengths) {
    if (length > std::numeric_limits<std::size_t>::max() - total) {
      throw FormatError{"context decoded symbol count overflow"};
    }
    total += length;
  }

  auto result = Model{};
  result.rules.resize(rule_lengths.size());
  if (total == 0) {
    return result;
  }

  auto models = BuildModels(frequencies);
  auto reader = arithmetic::BitReader{payload};
  auto decoder = arithmetic::detail::Decoder{32, reader};

  auto read_layer = [&](std::vector<Symbol>& layer, std::size_t count,
                        std::size_t context) {
    if (count == 0) {
      return;
    }
    if (context >= models.size() || !models[context].has_value()) {
      throw FormatError{"missing arithmetic model for decoded context"};
    }
    layer.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      layer.push_back(decoder.Read(*models[context]));
    }
  };

  if (layer_contexts.size() != rule_lengths.size() + 1) {
    throw FormatError{"context layer map has wrong size"};
  }
  read_layer(result.stream, stream_size, layer_contexts[0]);
  for (std::size_t i = 0; i < rule_lengths.size(); ++i) {
    read_layer(result.rules[i].symbols, rule_lengths[i],
               layer_contexts[i + 1]);
  }
  return result;
}

inline double IdealBits(
    std::vector<std::vector<std::uint32_t>> const& frequencies) {
  auto bits = 0.0;
  for (auto const& table : frequencies) {
    auto total = std::uint64_t{0};
    for (auto frequency : table) {
      total += frequency;
    }
    if (total == 0) {
      continue;
    }
    auto const total_double = static_cast<double>(total);
    for (auto frequency : table) {
      if (frequency == 0) {
        continue;
      }
      auto const probability =
          static_cast<double>(frequency) / total_double;
      bits -= static_cast<double>(frequency) * std::log2(probability);
    }
  }
  return bits;
}

inline std::vector<std::uint64_t> ExpectedContextTotals(
    std::size_t stream_size,
    std::vector<std::size_t> const& rule_lengths,
    std::vector<std::size_t> const& layer_contexts,
    std::size_t context_count) {
  if (layer_contexts.size() != rule_lengths.size() + 1) {
    throw FormatError{"context layer map has wrong size"};
  }
  auto totals = std::vector<std::uint64_t>(context_count, 0);
  if (layer_contexts[0] >= context_count) {
    throw FormatError{"stream context is out of range"};
  }
  totals[layer_contexts[0]] += stream_size;
  for (std::size_t i = 0; i < rule_lengths.size(); ++i) {
    auto const context = layer_contexts[i + 1];
    if (context >= context_count) {
      throw FormatError{"rule context is out of range"};
    }
    totals[context] += rule_lengths[i];
  }
  return totals;
}

}  // namespace context_detail

inline ContextFrame PackContextArithmetic(Model const& source,
                                          ContextScheme scheme) {
  auto plan = context_detail::BuildPlan(source, scheme);
  auto frequencies = context_detail::BuildFrequencies(plan);
  auto payload = context_detail::EncodePayload(plan, frequencies);

  auto out = std::vector<Byte>{};
  out.insert(out.end(), context_detail::kMagic.begin(),
             context_detail::kMagic.end());
  out.push_back(static_cast<Byte>(scheme));
  auto const fixed_header_bytes = out.size();

  auto const shape_begin = out.size();
  ae::compression::detail::WriteUvarint(out, plan.model.rules.size());
  ae::compression::detail::WriteUvarint(out, plan.model.stream.size());
  for (auto const& rule : plan.model.rules) {
    ae::compression::detail::WriteUvarint(out, rule.symbols.size());
  }
  if (context_detail::IsGrouped(scheme)) {
    ae::compression::detail::WriteUvarint(out,
                                           plan.group_rule_counts.size());
    for (auto count : plan.group_rule_counts) {
      ae::compression::detail::WriteUvarint(out, count);
    }
  }
  auto const shape_bytes = out.size() - shape_begin;

  auto non_zero_frequency_entries = std::size_t{0};
  auto largest_context_alphabet = std::size_t{0};
  auto const frequency_begin = out.size();
  for (auto const& table : frequencies) {
    auto non_zero = std::size_t{0};
    for (auto frequency : table) {
      if (frequency != 0) {
        ++non_zero;
      }
    }
    non_zero_frequency_entries += non_zero;
    largest_context_alphabet =
        std::max(largest_context_alphabet, non_zero);
    ae::compression::detail::WriteUvarint(out, non_zero);
    for (std::size_t symbol = 0; symbol < table.size(); ++symbol) {
      if (table[symbol] == 0) {
        continue;
      }
      ae::compression::detail::WriteSymbol(
          out, static_cast<Symbol>(symbol));
      ae::compression::detail::WriteUvarint(out, table[symbol]);
    }
  }
  auto const frequency_bytes = out.size() - frequency_begin;

  auto const payload_size_begin = out.size();
  ae::compression::detail::WriteUvarint(out, payload.size());
  auto const payload_size_bytes = out.size() - payload_size_begin;
  out.insert(out.end(), payload.begin(), payload.end());

  auto stats = ContextCodingStats{};
  stats.context_count = plan.context_count;
  stats.level_count = plan.group_rule_counts.size();
  stats.non_zero_frequency_entries = non_zero_frequency_entries;
  stats.largest_context_alphabet = largest_context_alphabet;
  stats.ideal_payload_bits = context_detail::IdealBits(frequencies);
  stats.payload_bytes = payload.size();
  stats.fixed_header_bytes = fixed_header_bytes;
  stats.shape_bytes = shape_bytes;
  stats.frequency_bytes = frequency_bytes;
  stats.payload_size_bytes = payload_size_bytes;
  stats.frame_bytes = out.size();
  return ContextFrame{std::move(out), stats};
}

inline Model UnpackContextArithmetic(std::span<Byte const> data) {
  if (data.size() < context_detail::kMagic.size() + 1) {
    throw FormatError{"context frame is too small"};
  }
  auto pos = std::size_t{0};
  for (auto expected : context_detail::kMagic) {
    if (data[pos++] != expected) {
      throw FormatError{"bad context frame magic"};
    }
  }

  auto const raw_scheme = data[pos++];
  if (raw_scheme > static_cast<Byte>(ContextScheme::kParentRule)) {
    throw FormatError{"unknown context coding scheme"};
  }
  auto const scheme = static_cast<ContextScheme>(raw_scheme);

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

  auto layer_contexts = std::vector<std::size_t>(rule_count + 1, 0);
  auto context_count = std::size_t{1};
  if (scheme == ContextScheme::kParentRule) {
    context_count = rule_count + 1;
    for (std::size_t i = 0; i < layer_contexts.size(); ++i) {
      layer_contexts[i] = i;
    }
  } else if (context_detail::IsGrouped(scheme)) {
    auto const level_count = context_detail::CheckedSize(
        ae::compression::detail::ReadUvarint(data, pos));
    context_count = level_count + 1;
    auto rule_pos = std::size_t{0};
    for (std::size_t level = 0; level < level_count; ++level) {
      auto const count = context_detail::CheckedSize(
          ae::compression::detail::ReadUvarint(data, pos));
      if (count > rule_count - rule_pos) {
        throw FormatError{"context level rule count overflow"};
      }
      for (std::size_t i = 0; i < count; ++i) {
        layer_contexts[rule_pos + i + 1] = level + 1;
      }
      rule_pos += count;
    }
    if (rule_pos != rule_count) {
      throw FormatError{"context level counts do not cover all rules"};
    }
  }

  if (rule_count >
      std::numeric_limits<std::size_t>::max() - kFirstRuleSymbol) {
    throw FormatError{"context alphabet size overflow"};
  }
  auto const alphabet_size =
      static_cast<std::size_t>(kFirstRuleSymbol) + rule_count;
  auto frequencies = std::vector<std::vector<std::uint32_t>>(
      context_count, std::vector<std::uint32_t>(alphabet_size, 0));

  for (std::size_t context = 0; context < context_count; ++context) {
    auto const non_zero_count = context_detail::CheckedSize(
        ae::compression::detail::ReadUvarint(data, pos));
    if (non_zero_count > alphabet_size) {
      throw FormatError{"too many context frequency entries"};
    }
    for (std::size_t i = 0; i < non_zero_count; ++i) {
      auto const symbol = ae::compression::detail::ReadSymbol(data, pos);
      auto const frequency =
          ae::compression::detail::ReadUvarint(data, pos);
      if (symbol >= alphabet_size || frequency == 0 ||
          frequency > std::numeric_limits<std::uint32_t>::max()) {
        throw FormatError{"bad context frequency entry"};
      }
      if (frequencies[context][symbol] != 0) {
        throw FormatError{"duplicate context frequency entry"};
      }
      frequencies[context][symbol] =
          static_cast<std::uint32_t>(frequency);
    }
  }

  auto const expected = context_detail::ExpectedContextTotals(
      stream_size, rule_lengths, layer_contexts, context_count);
  for (std::size_t context = 0; context < context_count; ++context) {
    auto total = std::uint64_t{0};
    for (auto frequency : frequencies[context]) {
      total += frequency;
    }
    if (total != expected[context]) {
      throw FormatError{"context frequency total mismatch"};
    }
  }

  auto const payload_size = context_detail::CheckedSize(
      ae::compression::detail::ReadUvarint(data, pos));
  if (data.size() - pos != payload_size) {
    throw FormatError{"context payload size mismatch"};
  }
  return context_detail::DecodePayload(
      data.subspan(pos, payload_size), stream_size, rule_lengths,
      layer_contexts, frequencies);
}

}  // namespace ae::compression::experimental

#endif  // AE_COMPRESSION_EXPERIMENTAL_CONTEXT_ARITHMETIC_HPP_
