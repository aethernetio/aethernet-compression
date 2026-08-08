/*
 * Copyright 2026 Aethernet Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#ifndef AE_COMPRESSION_EXPERIMENTAL_GRAMMAR_LZ_HPP_
#define AE_COMPRESSION_EXPERIMENTAL_GRAMMAR_LZ_HPP_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ae_compression/decoder.hpp"
#include "ae_compression/experimental/effective_compressor.hpp"
#include "ae_compression/format.hpp"
#include "ae_compression/model.hpp"

namespace ae::compression::experimental {

struct GrammarLzOptions {
  std::size_t max_candidates = 48;
  std::size_t max_chain = 96;
  std::size_t max_match = 4096;
};

struct GrammarLzStats {
  std::size_t grammar_rules = 0;
  std::size_t reachable_rules = 0;
  std::size_t candidate_rules = 0;
  std::size_t selected_rules = 0;
  std::size_t prefix_bytes = 0;
  std::size_t output_bytes = 0;
  std::size_t history_bytes = 0;
  std::size_t command_count = 0;
  std::size_t literal_commands = 0;
  std::size_t copy_commands = 0;
  std::size_t literal_bytes = 0;
  std::size_t copied_bytes = 0;
  std::size_t header_bytes = 0;
  std::size_t command_bytes = 0;
  std::size_t frame_bytes = 0;
};

struct GrammarLzFrame {
  std::vector<Byte> bytes;
  GrammarLzStats stats;
  std::vector<std::size_t> selected_rule_indices;
};

namespace grammar_lz_detail {

inline constexpr std::array<Byte, 4> kMagic = {'A', 'E', 'G', 'L'};
inline constexpr Byte kFlags = 0;
inline constexpr Byte kExtendedLiteral = 0xc0;
inline constexpr Byte kExtendedCopy = 0xc1;
inline constexpr std::size_t kShortLiteralLimit = 128;
inline constexpr std::size_t kShortCopyMinimum = 3;
inline constexpr std::size_t kShortCopyMaximum = 66;
inline constexpr std::size_t kHashBits = 16;
inline constexpr std::size_t kHashSize = std::size_t{1} << kHashBits;
inline constexpr std::size_t kDistanceBuckets = 10;

inline std::size_t UvarintSize(std::uint64_t value) {
  std::size_t size = 1;
  while (value >= 0x80) {
    value >>= 7;
    ++size;
  }
  return size;
}

inline std::size_t CheckedSize(std::uint64_t value) {
  if (value > std::numeric_limits<std::size_t>::max()) {
    throw FormatError{"grammar-lz value does not fit size_t"};
  }
  return static_cast<std::size_t>(value);
}

inline std::uint32_t Hash3(Byte first, Byte second, Byte third) {
  auto value = (static_cast<std::uint32_t>(first) << 16) |
               (static_cast<std::uint32_t>(second) << 8) |
               static_cast<std::uint32_t>(third);
  value *= 2654435761U;
  return value >> (32 - kHashBits);
}

inline std::size_t CopyCommandSize(std::size_t length,
                                   std::size_t distance) {
  if (length < kShortCopyMinimum || distance == 0) {
    return std::numeric_limits<std::size_t>::max();
  }
  auto const distance_bytes = UvarintSize(distance - 1);
  if (length <= kShortCopyMaximum) {
    return 1 + distance_bytes;
  }
  return 1 + UvarintSize(length) + distance_bytes;
}

struct Command {
  bool is_copy = false;
  std::vector<Byte> literals;
  std::size_t length = 0;
  std::size_t distance = 0;
};

struct Match {
  std::size_t length = 0;
  std::size_t distance = 0;
};

class CommandEncoder {
 public:
  explicit CommandEncoder(GrammarLzOptions options) : options_{options} {
    heads_.fill(-1);
  }

  void EncodeSegment(std::span<Byte const> target) {
    std::size_t position = 0;
    while (position < target.size()) {
      auto const matches = FindMatches(target, position);
      auto best = Match{};
      std::size_t best_gain = 0;
      for (auto const& match : matches) {
        if (match.length < kShortCopyMinimum) {
          continue;
        }
        auto const cost = CopyCommandSize(match.length, match.distance);
        if (cost == std::numeric_limits<std::size_t>::max() ||
            cost >= match.length) {
          continue;
        }
        auto const gain = match.length - cost;
        if (gain > best_gain ||
            (gain == best_gain && match.length > best.length) ||
            (gain == best_gain && match.length == best.length &&
             match.distance < best.distance)) {
          best = match;
          best_gain = gain;
        }
      }

      if (best.length >= kShortCopyMinimum) {
        AddCopy(best.length, best.distance);
        for (std::size_t i = 0; i < best.length; ++i) {
          AppendHistory(target[position + i]);
        }
        position += best.length;
        continue;
      }

      AddLiteral(target[position]);
      AppendHistory(target[position]);
      ++position;
    }
  }

  std::size_t HistorySize() const { return history_.size(); }

  std::vector<Byte> Serialize() const {
    auto out = std::vector<Byte>{};
    for (auto const& command : commands_) {
      if (!command.is_copy) {
        WriteLiteral(out, command.literals);
      } else {
        WriteCopy(out, command.length, command.distance);
      }
    }
    return out;
  }

  GrammarLzStats CommandStats() const {
    auto stats = GrammarLzStats{};
    stats.command_count = commands_.size();
    for (auto const& command : commands_) {
      if (command.is_copy) {
        ++stats.copy_commands;
        stats.copied_bytes += command.length;
      } else {
        ++stats.literal_commands;
        stats.literal_bytes += command.literals.size();
      }
    }
    stats.command_bytes = Serialize().size();
    return stats;
  }

 private:
  std::array<Match, kDistanceBuckets> FindMatches(
      std::span<Byte const> target, std::size_t position) const {
    auto result = std::array<Match, kDistanceBuckets>{};
    auto const remaining = target.size() - position;
    if (remaining < kShortCopyMinimum) {
      return result;
    }

    auto const hash = Hash3(target[position], target[position + 1],
                            target[position + 2]);
    auto candidate = heads_[hash];
    auto const current = history_.size();
    std::size_t chain = 0;
    while (candidate >= 0 && chain < options_.max_chain) {
      auto const source = static_cast<std::size_t>(candidate);
      if (source >= current) {
        break;
      }
      auto const distance = current - source;
      auto const limit = std::min(remaining, options_.max_match);
      std::size_t length = 0;
      while (length < limit) {
        Byte source_byte = 0;
        if (length < distance) {
          source_byte = history_[source + length];
        } else {
          source_byte = target[position + length - distance];
        }
        if (source_byte != target[position + length]) {
          break;
        }
        ++length;
      }

      if (length >= kShortCopyMinimum) {
        auto bucket = UvarintSize(distance - 1);
        if (bucket >= result.size()) {
          bucket = result.size() - 1;
        }
        if (length > result[bucket].length ||
            (length == result[bucket].length &&
             distance < result[bucket].distance)) {
          result[bucket] = Match{length, distance};
        }
      }

      candidate = previous_[source];
      ++chain;
    }
    return result;
  }

  void AppendHistory(Byte value) {
    history_.push_back(value);
    previous_.push_back(-1);
    if (history_.size() < kShortCopyMinimum) {
      return;
    }
    auto const position = history_.size() - kShortCopyMinimum;
    auto const hash = Hash3(history_[position], history_[position + 1],
                            history_[position + 2]);
    previous_[position] = heads_[hash];
    heads_[hash] = static_cast<std::int64_t>(position);
  }

  void AddLiteral(Byte value) {
    if (!commands_.empty() && !commands_.back().is_copy &&
        commands_.back().literals.size() < kShortLiteralLimit) {
      commands_.back().literals.push_back(value);
      return;
    }
    auto command = Command{};
    command.literals.push_back(value);
    commands_.push_back(std::move(command));
  }

  void AddCopy(std::size_t length, std::size_t distance) {
    if (!commands_.empty() && commands_.back().is_copy &&
        commands_.back().distance == distance) {
      commands_.back().length += length;
      return;
    }
    auto command = Command{};
    command.is_copy = true;
    command.length = length;
    command.distance = distance;
    commands_.push_back(std::move(command));
  }

  static void WriteLiteral(std::vector<Byte>& out,
                           std::vector<Byte> const& literals) {
    if (literals.empty()) {
      throw FormatError{"grammar-lz literal command is empty"};
    }
    if (literals.size() <= kShortLiteralLimit) {
      out.push_back(static_cast<Byte>(literals.size() - 1));
    } else {
      out.push_back(kExtendedLiteral);
      ae::compression::detail::WriteUvarint(out, literals.size());
    }
    out.insert(out.end(), literals.begin(), literals.end());
  }

  static void WriteCopy(std::vector<Byte>& out, std::size_t length,
                        std::size_t distance) {
    if (length < kShortCopyMinimum || distance == 0) {
      throw FormatError{"bad grammar-lz copy command"};
    }
    if (length <= kShortCopyMaximum) {
      out.push_back(static_cast<Byte>(
          0x80U + (length - kShortCopyMinimum)));
    } else {
      out.push_back(kExtendedCopy);
      ae::compression::detail::WriteUvarint(out, length);
    }
    ae::compression::detail::WriteUvarint(out, distance - 1);
  }

  GrammarLzOptions options_;
  std::vector<Command> commands_;
  std::vector<Byte> history_;
  std::array<std::int64_t, kHashSize> heads_{};
  std::vector<std::int64_t> previous_;
};

struct Candidate {
  std::size_t rule_index = 0;
  Weight score = 0;
  std::size_t length = 0;
  Weight multiplicity = 0;
};

inline Weight CandidateScore(std::size_t length, Weight multiplicity) {
  if (multiplicity <= 1 || length == 0) {
    return 0;
  }
  return detail::SaturatingMultiply(
      static_cast<Weight>(length), multiplicity - 1);
}

}  // namespace grammar_lz_detail

class GrammarLzOptimizer {
 public:
  explicit GrammarLzOptimizer(Model const& model,
                              GrammarLzOptions options = {})
      : model_{model}, options_{options} {
    Analyze();
  }

  std::size_t CandidateCount() const { return candidates_.size(); }

  std::vector<std::size_t> TrialRuleLimits() const {
    auto limits = std::vector<std::size_t>{0};
    auto const count = candidates_.size();
    for (std::size_t i = 1; i <= std::min<std::size_t>(count, 12); ++i) {
      limits.push_back(i);
    }
    for (auto value : {std::size_t{16}, std::size_t{24},
                       std::size_t{32}, std::size_t{48}}) {
      if (value <= count) {
        limits.push_back(value);
      }
    }
    if (count != 0) {
      limits.push_back(count);
    }
    std::sort(limits.begin(), limits.end());
    limits.erase(std::unique(limits.begin(), limits.end()), limits.end());
    return limits;
  }

  GrammarLzFrame PackPlain() const { return PackForRuleLimit(0); }

  GrammarLzFrame PackAllCandidates() const {
    return PackForRuleLimit(candidates_.size());
  }

  GrammarLzFrame PackBest() const {
    auto best = GrammarLzFrame{};
    bool initialized = false;
    for (auto limit : TrialRuleLimits()) {
      auto frame = PackForRuleLimit(limit);
      if (!initialized || frame.bytes.size() < best.bytes.size() ||
          (frame.bytes.size() == best.bytes.size() &&
           frame.stats.prefix_bytes < best.stats.prefix_bytes)) {
        best = std::move(frame);
        initialized = true;
      }
    }
    return best;
  }

  GrammarLzFrame PackForRuleLimit(std::size_t limit) const {
    limit = std::min(limit, candidates_.size());
    auto selected = std::vector<std::uint8_t>(model_.rules.size(), 0);
    auto selected_indices = std::vector<std::size_t>{};
    selected_indices.reserve(limit);
    for (std::size_t i = 0; i < limit; ++i) {
      auto const rule = candidates_[i].rule_index;
      selected[rule] = 1;
      selected_indices.push_back(rule);
    }

    auto encoder = grammar_lz_detail::CommandEncoder{options_};
    for (auto rule : topological_rules_) {
      if (selected[rule] == 0) {
        continue;
      }
      encoder.EncodeSegment(rule_expansions_[rule]);
    }
    auto const prefix_size = encoder.HistorySize();
    encoder.EncodeSegment(root_);

    auto commands = encoder.Serialize();
    auto out = std::vector<Byte>{};
    out.insert(out.end(), grammar_lz_detail::kMagic.begin(),
               grammar_lz_detail::kMagic.end());
    out.push_back(grammar_lz_detail::kFlags);
    ae::compression::detail::WriteUvarint(out, prefix_size);
    ae::compression::detail::WriteUvarint(out, root_.size());
    auto const header_bytes = out.size();
    out.insert(out.end(), commands.begin(), commands.end());

    auto stats = encoder.CommandStats();
    stats.grammar_rules = model_.rules.size();
    stats.reachable_rules = topological_rules_.size();
    stats.candidate_rules = candidates_.size();
    stats.selected_rules = limit;
    stats.prefix_bytes = prefix_size;
    stats.output_bytes = root_.size();
    stats.history_bytes = prefix_size + root_.size();
    stats.header_bytes = header_bytes;
    stats.command_bytes = commands.size();
    stats.frame_bytes = out.size();
    return GrammarLzFrame{std::move(out), std::move(stats),
                          std::move(selected_indices)};
  }

  std::vector<Byte> BuildPrefixForRuleLimit(std::size_t limit) const {
    limit = std::min(limit, candidates_.size());
    auto selected = std::vector<std::uint8_t>(model_.rules.size(), 0);
    for (std::size_t i = 0; i < limit; ++i) {
      selected[candidates_[i].rule_index] = 1;
    }
    auto prefix = std::vector<Byte>{};
    for (auto rule : topological_rules_) {
      if (selected[rule] == 0) {
        continue;
      }
      auto const& expansion = rule_expansions_[rule];
      if (expansion.size() >
          std::numeric_limits<std::size_t>::max() - prefix.size()) {
        throw FormatError{"grammar-lz prefix size overflow"};
      }
      prefix.insert(prefix.end(), expansion.begin(), expansion.end());
    }
    return prefix;
  }

  std::span<Byte const> RootBytes() const { return root_; }

 private:
  void Analyze() {
    rule_expansions_.resize(model_.rules.size());
    states_.assign(model_.rules.size(), 0);
    reachable_.assign(model_.rules.size(), 0);

    for (auto symbol : model_.stream) {
      if (symbol >= kFirstRuleSymbol) {
        auto const rule = RuleIndex(symbol);
        ExpandRule(rule);
      }
    }
    root_ = ExpandLayer(model_.stream);

    auto multiplicities = RuleMultiplicities(model_);
    auto deduplicated = std::map<std::vector<Byte>,
                                 grammar_lz_detail::Candidate>{};
    for (auto rule : topological_rules_) {
      auto const& expansion = rule_expansions_[rule];
      auto const multiplicity =
          rule < multiplicities.size() ? multiplicities[rule] : 0;
      auto const score = grammar_lz_detail::CandidateScore(
          expansion.size(), multiplicity);
      if (score == 0 ||
          expansion.size() < grammar_lz_detail::kShortCopyMinimum) {
        continue;
      }
      auto candidate = grammar_lz_detail::Candidate{
          rule, score, expansion.size(), multiplicity};
      auto found = deduplicated.find(expansion);
      if (found == deduplicated.end() ||
          candidate.score > found->second.score ||
          (candidate.score == found->second.score &&
           candidate.rule_index < found->second.rule_index)) {
        deduplicated[expansion] = candidate;
      }
    }

    candidates_.reserve(deduplicated.size());
    for (auto const& [expansion, candidate] : deduplicated) {
      (void)expansion;
      candidates_.push_back(candidate);
    }
    std::sort(candidates_.begin(), candidates_.end(),
              [](grammar_lz_detail::Candidate const& left,
                 grammar_lz_detail::Candidate const& right) {
                if (left.score != right.score) {
                  return left.score > right.score;
                }
                if (left.length != right.length) {
                  return left.length > right.length;
                }
                return left.rule_index < right.rule_index;
              });
    if (candidates_.size() > options_.max_candidates) {
      candidates_.resize(options_.max_candidates);
    }
  }

  std::size_t RuleIndex(Symbol symbol) const {
    auto const index =
        static_cast<std::size_t>(symbol - kFirstRuleSymbol);
    if (index >= model_.rules.size()) {
      throw FormatError{"grammar-lz references an unknown rule"};
    }
    return index;
  }

  std::vector<Byte> ExpandLayer(std::vector<Symbol> const& symbols) {
    auto result = std::vector<Byte>{};
    for (auto symbol : symbols) {
      if (symbol < kFirstRuleSymbol) {
        result.push_back(static_cast<Byte>(symbol));
        continue;
      }
      auto const rule = RuleIndex(symbol);
      auto const& expansion = ExpandRule(rule);
      if (expansion.size() >
          std::numeric_limits<std::size_t>::max() - result.size()) {
        throw FormatError{"grammar-lz expansion size overflow"};
      }
      result.insert(result.end(), expansion.begin(), expansion.end());
    }
    return result;
  }

  std::vector<Byte> const& ExpandRule(std::size_t rule) {
    if (states_[rule] == 2) {
      return rule_expansions_[rule];
    }
    if (states_[rule] == 1) {
      throw FormatError{"grammar-lz rule graph is cyclic"};
    }
    states_[rule] = 1;
    reachable_[rule] = 1;
    rule_expansions_[rule] = ExpandLayer(model_.rules[rule].symbols);
    states_[rule] = 2;
    topological_rules_.push_back(rule);
    return rule_expansions_[rule];
  }

  Model const& model_;
  GrammarLzOptions options_;
  std::vector<std::vector<Byte>> rule_expansions_;
  std::vector<std::uint8_t> states_;
  std::vector<std::uint8_t> reachable_;
  std::vector<std::size_t> topological_rules_;
  std::vector<grammar_lz_detail::Candidate> candidates_;
  std::vector<Byte> root_;
};

inline std::vector<Byte> DecodeGrammarLz(std::span<Byte const> data) {
  if (data.size() < grammar_lz_detail::kMagic.size() + 1) {
    throw FormatError{"grammar-lz frame is too small"};
  }
  std::size_t position = 0;
  for (auto expected : grammar_lz_detail::kMagic) {
    if (data[position++] != expected) {
      throw FormatError{"bad grammar-lz frame magic"};
    }
  }
  if (data[position++] != grammar_lz_detail::kFlags) {
    throw FormatError{"unsupported grammar-lz flags"};
  }

  auto const prefix_size = grammar_lz_detail::CheckedSize(
      ae::compression::detail::ReadUvarint(data, position));
  auto const output_size = grammar_lz_detail::CheckedSize(
      ae::compression::detail::ReadUvarint(data, position));
  if (prefix_size >
      std::numeric_limits<std::size_t>::max() - output_size) {
    throw FormatError{"grammar-lz output size overflow"};
  }
  auto const total_size = prefix_size + output_size;
  auto history = std::vector<Byte>{};
  history.reserve(total_size);

  while (history.size() < total_size) {
    if (position >= data.size()) {
      throw FormatError{"truncated grammar-lz command stream"};
    }
    auto const token = data[position++];
    if (token <= 0x7f) {
      auto const length = static_cast<std::size_t>(token) + 1;
      if (length > total_size - history.size() ||
          length > data.size() - position) {
        throw FormatError{"bad grammar-lz literal length"};
      }
      history.insert(history.end(),
                     data.begin() + static_cast<std::ptrdiff_t>(position),
                     data.begin() +
                         static_cast<std::ptrdiff_t>(position + length));
      position += length;
      continue;
    }

    std::size_t length = 0;
    if (token >= 0x80 && token <= 0xbf) {
      length = grammar_lz_detail::kShortCopyMinimum +
               static_cast<std::size_t>(token - 0x80);
    } else if (token == grammar_lz_detail::kExtendedLiteral) {
      length = grammar_lz_detail::CheckedSize(
          ae::compression::detail::ReadUvarint(data, position));
      if (length == 0 || length > total_size - history.size() ||
          length > data.size() - position) {
        throw FormatError{"bad extended grammar-lz literal length"};
      }
      history.insert(history.end(),
                     data.begin() + static_cast<std::ptrdiff_t>(position),
                     data.begin() +
                         static_cast<std::ptrdiff_t>(position + length));
      position += length;
      continue;
    } else if (token == grammar_lz_detail::kExtendedCopy) {
      length = grammar_lz_detail::CheckedSize(
          ae::compression::detail::ReadUvarint(data, position));
    } else {
      throw FormatError{"unknown grammar-lz command token"};
    }

    auto const encoded_distance =
        ae::compression::detail::ReadUvarint(data, position);
    if (encoded_distance == std::numeric_limits<std::uint64_t>::max()) {
      throw FormatError{"grammar-lz distance overflow"};
    }
    auto const distance =
        grammar_lz_detail::CheckedSize(encoded_distance + 1);
    if (length < grammar_lz_detail::kShortCopyMinimum ||
        length > total_size - history.size() || distance > history.size()) {
      throw FormatError{"bad grammar-lz copy command"};
    }
    for (std::size_t i = 0; i < length; ++i) {
      history.push_back(history[history.size() - distance]);
    }
  }

  if (position != data.size()) {
    throw FormatError{"trailing bytes after grammar-lz frame"};
  }
  return std::vector<Byte>{
      history.begin() + static_cast<std::ptrdiff_t>(prefix_size),
      history.end()};
}

inline GrammarLzFrame PackGrammarLz(
    Model const& model, GrammarLzOptions options = {}) {
  return GrammarLzOptimizer{model, options}.PackBest();
}

inline GrammarLzFrame PackPlainLz(
    std::span<Byte const> data, GrammarLzOptions options = {}) {
  auto model = Model{};
  model.stream.reserve(data.size());
  for (auto byte : data) {
    model.stream.push_back(byte);
  }
  return GrammarLzOptimizer{model, options}.PackPlain();
}

}  // namespace ae::compression::experimental

#endif  // AE_COMPRESSION_EXPERIMENTAL_GRAMMAR_LZ_HPP_
