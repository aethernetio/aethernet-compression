/*
 * Copyright 2026 Aethernet Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#ifndef AE_COMPRESSION_EXPERIMENTAL_CLASS_PRIOR_ARITHMETIC_HPP_
#define AE_COMPRESSION_EXPERIMENTAL_CLASS_PRIOR_ARITHMETIC_HPP_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "ae_compression/experimental/prior_residual_arithmetic.hpp"

namespace ae::compression::experimental {

struct ClassPriorStats {
  std::size_t context_count = 0;
  std::size_t level_count = 0;
  std::size_t class_count = 0;
  std::size_t global_frequency_entries = 0;
  std::size_t actual_frequency_entries = 0;
  std::size_t model_frequency_entries = 0;
  std::size_t stored_class_counts = 0;
  std::size_t largest_context_alphabet = 0;
  double ideal_payload_bits = 0.0;
  std::size_t payload_bytes = 0;
  std::size_t fixed_header_bytes = 0;
  std::size_t shape_bytes = 0;
  std::size_t prior_bytes = 0;
  std::size_t class_count_bytes = 0;
  std::size_t payload_size_bytes = 0;
  std::size_t frame_bytes = 0;
};

struct ClassPriorFrame {
  std::vector<Byte> bytes;
  ClassPriorStats stats;
};

namespace class_prior_detail {

inline constexpr std::array<Byte, 4> kMagic = {'A', 'E', 'C', 'P'};
inline constexpr std::uint32_t kModelTotal = 65536;

struct SymbolRange {
  std::size_t begin = 0;
  std::size_t end = 0;
};

inline std::vector<SymbolRange> BuildClassRanges(
    std::size_t rule_count,
    std::vector<std::size_t> const& group_rule_counts) {
  auto ranges = std::vector<SymbolRange>{};
  ranges.reserve(group_rule_counts.size() + 1);
  ranges.push_back(SymbolRange{0, kFirstRuleSymbol});

  auto position = static_cast<std::size_t>(kFirstRuleSymbol);
  for (auto count : group_rule_counts) {
    if (count > rule_count || position - kFirstRuleSymbol > rule_count - count) {
      throw FormatError{"class-prior rule group exceeds rule count"};
    }
    ranges.push_back(SymbolRange{position, position + count});
    position += count;
  }
  if (position != static_cast<std::size_t>(kFirstRuleSymbol) + rule_count) {
    throw FormatError{"class-prior groups do not cover the rule alphabet"};
  }
  return ranges;
}

inline std::size_t AllowedClassCount(std::size_t level_count,
                                     std::size_t context) {
  if (context == 0) {
    return level_count + 1;
  }
  if (context > level_count) {
    throw FormatError{"class-prior context exceeds level count"};
  }
  // Context 1 contains height-0 rule bodies and therefore only literals.
  // Context N contains height-(N-1) bodies and may reference lower heights.
  return context;
}

inline std::vector<std::vector<std::uint64_t>> BuildClassCounts(
    std::vector<std::vector<std::uint32_t>> const& actual,
    std::vector<SymbolRange> const& ranges) {
  if (actual.empty()) {
    return {};
  }
  auto result = std::vector<std::vector<std::uint64_t>>(
      actual.size(), std::vector<std::uint64_t>(ranges.size(), 0));
  for (std::size_t context = 0; context < actual.size(); ++context) {
    for (std::size_t class_index = 0; class_index < ranges.size();
         ++class_index) {
      auto const range = ranges[class_index];
      if (range.end > actual[context].size()) {
        throw FormatError{"class-prior range exceeds frequency alphabet"};
      }
      auto total = std::uint64_t{0};
      for (std::size_t symbol = range.begin; symbol < range.end; ++symbol) {
        total += actual[context][symbol];
      }
      result[context][class_index] = total;
    }
  }
  return result;
}

struct AllocationRemainder {
  std::uint64_t remainder = 0;
  std::size_t index = 0;
};

inline std::vector<std::uint32_t> AllocateClassModel(
    std::vector<std::uint32_t> const& global,
    std::vector<SymbolRange> const& ranges,
    std::span<std::uint64_t const> class_counts,
    std::size_t allowed_classes, std::uint64_t context_total) {
  auto result = std::vector<std::uint32_t>(global.size(), 0);
  if (context_total == 0) {
    return result;
  }
  if (allowed_classes == 0 || allowed_classes > ranges.size() ||
      class_counts.size() != ranges.size()) {
    throw FormatError{"bad class-prior class count"};
  }

  auto support_counts = std::vector<std::size_t>(allowed_classes, 0);
  auto global_class_totals =
      std::vector<std::uint64_t>(allowed_classes, 0);
  auto base_total = std::size_t{0};
  auto checked_context_total = std::uint64_t{0};
  for (std::size_t class_index = 0; class_index < allowed_classes;
       ++class_index) {
    checked_context_total += class_counts[class_index];
    auto const range = ranges[class_index];
    for (std::size_t symbol = range.begin; symbol < range.end; ++symbol) {
      global_class_totals[class_index] += global[symbol];
      if (global[symbol] != 0) {
        ++support_counts[class_index];
      }
    }
    if (class_counts[class_index] != 0) {
      if (support_counts[class_index] == 0 ||
          global_class_totals[class_index] == 0) {
        throw FormatError{"active class has no global-prior support"};
      }
      base_total += support_counts[class_index];
    }
  }
  for (std::size_t class_index = allowed_classes;
       class_index < class_counts.size(); ++class_index) {
    if (class_counts[class_index] != 0) {
      throw FormatError{"disallowed class has a non-zero count"};
    }
  }
  if (checked_context_total != context_total) {
    throw FormatError{"class-prior counts do not match context total"};
  }
  if (base_total > kModelTotal) {
    throw FormatError{"class-prior support exceeds model total"};
  }

  auto class_budgets = std::vector<std::uint32_t>(allowed_classes, 0);
  auto remaining =
      static_cast<std::uint64_t>(kModelTotal - base_total);
  auto assigned = std::uint64_t{0};
  auto class_remainders = std::vector<AllocationRemainder>{};
  class_remainders.reserve(allowed_classes);
  for (std::size_t class_index = 0; class_index < allowed_classes;
       ++class_index) {
    if (class_counts[class_index] == 0) {
      continue;
    }
    auto const product = remaining * class_counts[class_index];
    auto const extra = product / context_total;
    class_budgets[class_index] = static_cast<std::uint32_t>(
        support_counts[class_index] + extra);
    assigned += extra;
    class_remainders.push_back(
        AllocationRemainder{product % context_total, class_index});
  }
  if (assigned > remaining) {
    throw FormatError{"class-prior class allocation overflow"};
  }
  auto class_left = remaining - assigned;
  std::sort(class_remainders.begin(), class_remainders.end(),
            [](AllocationRemainder const& left,
               AllocationRemainder const& right) {
              if (left.remainder != right.remainder) {
                return left.remainder > right.remainder;
              }
              return left.index < right.index;
            });
  if (class_left > class_remainders.size()) {
    throw FormatError{"class-prior class rounding overflow"};
  }
  for (std::size_t i = 0; i < static_cast<std::size_t>(class_left); ++i) {
    ++class_budgets[class_remainders[i].index];
  }

  for (std::size_t class_index = 0; class_index < allowed_classes;
       ++class_index) {
    if (class_counts[class_index] == 0) {
      continue;
    }
    auto const range = ranges[class_index];
    auto class_remaining = static_cast<std::uint64_t>(
        class_budgets[class_index] - support_counts[class_index]);
    auto symbol_assigned = std::uint64_t{0};
    auto symbol_remainders = std::vector<AllocationRemainder>{};
    symbol_remainders.reserve(support_counts[class_index]);

    for (std::size_t symbol = range.begin; symbol < range.end; ++symbol) {
      auto const weight = global[symbol];
      if (weight == 0) {
        continue;
      }
      result[symbol] = 1;
      auto const product = class_remaining * weight;
      auto const extra = product / global_class_totals[class_index];
      if (extra > std::numeric_limits<std::uint32_t>::max() - 1) {
        throw FormatError{"class-prior symbol allocation overflow"};
      }
      result[symbol] += static_cast<std::uint32_t>(extra);
      symbol_assigned += extra;
      symbol_remainders.push_back(AllocationRemainder{
          product % global_class_totals[class_index], symbol});
    }
    if (symbol_assigned > class_remaining) {
      throw FormatError{"class-prior symbol assignment overflow"};
    }
    auto symbol_left = class_remaining - symbol_assigned;
    std::sort(symbol_remainders.begin(), symbol_remainders.end(),
              [](AllocationRemainder const& left,
                 AllocationRemainder const& right) {
                if (left.remainder != right.remainder) {
                  return left.remainder > right.remainder;
                }
                return left.index < right.index;
              });
    if (symbol_left > symbol_remainders.size()) {
      throw FormatError{"class-prior symbol rounding overflow"};
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(symbol_left); ++i) {
      ++result[symbol_remainders[i].index];
    }
  }

  if (prior_detail::SumTable(result) != kModelTotal) {
    throw FormatError{"class-prior model has the wrong total"};
  }
  return result;
}

inline std::vector<std::vector<std::uint32_t>> BuildModels(
    std::vector<std::uint32_t> const& global,
    std::vector<SymbolRange> const& ranges,
    std::vector<std::vector<std::uint64_t>> const& class_counts,
    std::vector<std::uint64_t> const& context_totals) {
  if (class_counts.size() != context_totals.size()) {
    throw FormatError{"class-prior context count mismatch"};
  }
  auto result = std::vector<std::vector<std::uint32_t>>{};
  result.reserve(context_totals.size());
  auto const level_count = ranges.size() - 1;
  for (std::size_t context = 0; context < context_totals.size(); ++context) {
    result.push_back(AllocateClassModel(
        global, ranges, class_counts[context],
        AllowedClassCount(level_count, context), context_totals[context]));
  }
  return result;
}

inline std::size_t WriteClassCounts(
    std::vector<Byte>& out,
    std::vector<std::vector<std::uint64_t>> const& class_counts,
    std::vector<std::uint64_t> const& context_totals) {
  if (class_counts.size() != context_totals.size() || class_counts.empty()) {
    throw FormatError{"class-prior count table mismatch"};
  }
  auto const level_count = class_counts.front().size() - 1;
  auto stored = std::size_t{0};
  for (std::size_t context = 0; context < class_counts.size(); ++context) {
    auto const allowed = AllowedClassCount(level_count, context);
    auto sum = std::uint64_t{0};
    for (std::size_t class_index = 0; class_index + 1 < allowed;
         ++class_index) {
      ae::compression::detail::WriteUvarint(
          out, class_counts[context][class_index]);
      sum += class_counts[context][class_index];
      ++stored;
    }
    if (sum > context_totals[context] ||
        class_counts[context][allowed - 1] !=
            context_totals[context] - sum) {
      throw FormatError{"class-prior omitted class count mismatch"};
    }
    for (std::size_t class_index = allowed;
         class_index < class_counts[context].size(); ++class_index) {
      if (class_counts[context][class_index] != 0) {
        throw FormatError{"class-prior disallowed class is non-zero"};
      }
    }
  }
  return stored;
}

inline std::vector<std::vector<std::uint64_t>> ReadClassCounts(
    std::span<Byte const> data, std::size_t& pos, std::size_t level_count,
    std::vector<std::uint64_t> const& context_totals) {
  auto result = std::vector<std::vector<std::uint64_t>>(
      context_totals.size(), std::vector<std::uint64_t>(level_count + 1, 0));
  for (std::size_t context = 0; context < context_totals.size(); ++context) {
    auto const allowed = AllowedClassCount(level_count, context);
    auto sum = std::uint64_t{0};
    for (std::size_t class_index = 0; class_index + 1 < allowed;
         ++class_index) {
      auto const count =
          ae::compression::detail::ReadUvarint(data, pos);
      if (count > context_totals[context] - sum) {
        throw FormatError{"class-prior count exceeds context total"};
      }
      result[context][class_index] = count;
      sum += count;
    }
    result[context][allowed - 1] = context_totals[context] - sum;
  }
  return result;
}

inline bool SameClassCounts(
    std::vector<std::vector<std::uint64_t>> const& left,
    std::vector<std::vector<std::uint64_t>> const& right) {
  return left == right;
}

}  // namespace class_prior_detail

inline ClassPriorFrame PackClassPrior(Model const& source) {
  auto plan = context_detail::BuildPlan(source, ContextScheme::kRuleHeight);
  auto actual = context_detail::BuildFrequencies(plan);
  auto global = prior_detail::SumFrequencies(actual);
  auto ranges = class_prior_detail::BuildClassRanges(
      plan.model.rules.size(), plan.group_rule_counts);
  auto class_counts =
      class_prior_detail::BuildClassCounts(actual, ranges);

  auto rule_lengths = std::vector<std::size_t>{};
  rule_lengths.reserve(plan.model.rules.size());
  for (auto const& rule : plan.model.rules) {
    rule_lengths.push_back(rule.symbols.size());
  }
  auto context_totals = context_detail::ExpectedContextTotals(
      plan.model.stream.size(), rule_lengths, plan.layer_contexts,
      plan.context_count);
  auto models = class_prior_detail::BuildModels(
      global, ranges, class_counts, context_totals);
  auto payload = context_detail::EncodePayload(plan, models);

  auto out = std::vector<Byte>{};
  out.insert(out.end(), class_prior_detail::kMagic.begin(),
             class_prior_detail::kMagic.end());
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

  auto const class_begin = out.size();
  auto const stored_class_counts = class_prior_detail::WriteClassCounts(
      out, class_counts, context_totals);
  auto const class_count_bytes = out.size() - class_begin;

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
  auto stats = ClassPriorStats{};
  stats.context_count = plan.context_count;
  stats.level_count = plan.group_rule_counts.size();
  stats.class_count = ranges.size();
  stats.global_frequency_entries = global_entries;
  stats.actual_frequency_entries = prior_detail::CountNonZero(actual);
  stats.model_frequency_entries = prior_detail::CountNonZero(models);
  stats.stored_class_counts = stored_class_counts;
  stats.largest_context_alphabet = prior_detail::LargestAlphabet(models);
  stats.ideal_payload_bits = prior_detail::CrossEntropyBits(actual, models);
  stats.payload_bytes = payload.size();
  stats.fixed_header_bytes = fixed_header_bytes;
  stats.shape_bytes = shape_bytes;
  stats.prior_bytes = prior_bytes;
  stats.class_count_bytes = class_count_bytes;
  stats.payload_size_bytes = payload_size_bytes;
  stats.frame_bytes = out.size();
  return ClassPriorFrame{std::move(out), stats};
}

inline Model UnpackClassPrior(std::span<Byte const> data) {
  if (data.size() < class_prior_detail::kMagic.size()) {
    throw FormatError{"class-prior frame is too small"};
  }
  auto pos = std::size_t{0};
  for (auto expected : class_prior_detail::kMagic) {
    if (data[pos++] != expected) {
      throw FormatError{"bad class-prior frame magic"};
    }
  }

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
      throw FormatError{"class-prior level rule count overflow"};
    }
    group_rule_counts.push_back(count);
    covered_rules += count;
  }
  if (covered_rules != rule_count) {
    throw FormatError{"class-prior levels do not cover all rules"};
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
    throw FormatError{"class-prior total does not match model shape"};
  }

  auto ranges = class_prior_detail::BuildClassRanges(
      rule_count, group_rule_counts);
  auto class_counts = class_prior_detail::ReadClassCounts(
      data, pos, level_count, context_totals);
  auto models = class_prior_detail::BuildModels(
      global, ranges, class_counts, context_totals);

  auto const payload_size = context_detail::CheckedSize(
      ae::compression::detail::ReadUvarint(data, pos));
  if (payload_size != data.size() - pos) {
    throw FormatError{"class-prior payload size mismatch"};
  }
  auto model = context_detail::DecodePayload(
      data.subspan(pos, payload_size), stream_size, rule_lengths,
      layer_contexts, models);
  if (prior_detail::CountDecodedGlobal(model) != global) {
    throw FormatError{"decoded class-prior model does not match global counts"};
  }
  auto decoded_actual = context_detail::BuildFrequencies(
      context_detail::ContextPlan{model, layer_contexts, group_rule_counts,
                                  context_count});
  auto decoded_class_counts =
      class_prior_detail::BuildClassCounts(decoded_actual, ranges);
  if (!class_prior_detail::SameClassCounts(decoded_class_counts,
                                            class_counts)) {
    throw FormatError{"decoded class-prior model does not match class counts"};
  }
  return model;
}

}  // namespace ae::compression::experimental

#endif  // AE_COMPRESSION_EXPERIMENTAL_CLASS_PRIOR_ARITHMETIC_HPP_
