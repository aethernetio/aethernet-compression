/*
 * Copyright 2026 Aethernet Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#ifndef AE_COMPRESSION_EXPERIMENTAL_GRAMMAR_LZ_LAYOUT_HPP_
#define AE_COMPRESSION_EXPERIMENTAL_GRAMMAR_LZ_LAYOUT_HPP_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include "ae_compression/experimental/grammar_lz.hpp"

namespace ae::compression::experimental {

enum class GrammarDictionaryLayout : std::uint8_t {
  kContainment = 0,
  kGreedySuperstring = 1,
};

inline char const* GrammarDictionaryLayoutName(
    GrammarDictionaryLayout layout) {
  switch (layout) {
    case GrammarDictionaryLayout::kContainment:
      return "containment";
    case GrammarDictionaryLayout::kGreedySuperstring:
      return "greedy_superstring";
  }
  return "unknown";
}

namespace grammar_layout_detail {

struct DictionaryLayout {
  std::vector<Byte> bytes;
  std::size_t fragments = 0;
};

inline bool Contains(std::vector<Byte> const& haystack,
                     std::vector<Byte> const& needle) {
  if (needle.empty()) {
    return true;
  }
  if (needle.size() > haystack.size()) {
    return false;
  }
  return std::search(haystack.begin(), haystack.end(), needle.begin(),
                     needle.end()) != haystack.end();
}

inline std::vector<std::vector<Byte>> RemoveContained(
    std::vector<std::vector<Byte>> strings) {
  auto keep = std::vector<std::uint8_t>(strings.size(), 1);
  for (std::size_t i = 0; i < strings.size(); ++i) {
    if (keep[i] == 0) {
      continue;
    }
    for (std::size_t j = 0; j < strings.size(); ++j) {
      if (i == j || strings[j].size() < strings[i].size()) {
        continue;
      }
      if (strings[j].size() == strings[i].size() && j > i) {
        continue;
      }
      if (Contains(strings[j], strings[i])) {
        keep[i] = 0;
        break;
      }
    }
  }

  auto result = std::vector<std::vector<Byte>>{};
  result.reserve(strings.size());
  for (std::size_t i = 0; i < strings.size(); ++i) {
    if (keep[i] != 0) {
      result.push_back(std::move(strings[i]));
    }
  }
  return result;
}

inline std::size_t MaximumOverlap(std::vector<Byte> const& left,
                                  std::vector<Byte> const& right) {
  auto length = std::min(left.size(), right.size());
  while (length != 0) {
    if (std::equal(left.end() - static_cast<std::ptrdiff_t>(length),
                   left.end(), right.begin())) {
      return length;
    }
    --length;
  }
  return 0;
}

inline DictionaryLayout ConcatenateMaximal(
    std::vector<std::vector<Byte>> strings) {
  strings = RemoveContained(std::move(strings));
  auto result = DictionaryLayout{};
  result.fragments = strings.size();
  for (auto const& string : strings) {
    if (string.size() >
        std::numeric_limits<std::size_t>::max() - result.bytes.size()) {
      throw FormatError{"grammar dictionary size overflow"};
    }
    result.bytes.insert(result.bytes.end(), string.begin(), string.end());
  }
  return result;
}

inline DictionaryLayout GreedySuperstring(
    std::vector<std::vector<Byte>> strings) {
  strings = RemoveContained(std::move(strings));
  if (strings.empty()) {
    return {};
  }

  while (strings.size() > 1) {
    std::size_t best_left = 0;
    std::size_t best_right = 1;
    std::size_t best_overlap = 0;
    std::size_t best_merged_size =
        strings[0].size() + strings[1].size();

    for (std::size_t left = 0; left < strings.size(); ++left) {
      for (std::size_t right = 0; right < strings.size(); ++right) {
        if (left == right) {
          continue;
        }
        auto const overlap = MaximumOverlap(strings[left], strings[right]);
        auto const merged_size =
            strings[left].size() + strings[right].size() - overlap;
        if (overlap > best_overlap ||
            (overlap == best_overlap && merged_size < best_merged_size) ||
            (overlap == best_overlap && merged_size == best_merged_size &&
             std::pair{left, right} <
                 std::pair{best_left, best_right})) {
          best_left = left;
          best_right = right;
          best_overlap = overlap;
          best_merged_size = merged_size;
        }
      }
    }

    auto merged = strings[best_left];
    merged.insert(
        merged.end(),
        strings[best_right].begin() +
            static_cast<std::ptrdiff_t>(best_overlap),
        strings[best_right].end());

    auto next = std::vector<std::vector<Byte>>{};
    next.reserve(strings.size() - 1);
    for (std::size_t i = 0; i < strings.size(); ++i) {
      if (i != best_left && i != best_right) {
        next.push_back(std::move(strings[i]));
      }
    }
    next.push_back(std::move(merged));
    strings = RemoveContained(std::move(next));
  }

  return DictionaryLayout{std::move(strings.front()), 1};
}

inline DictionaryLayout BuildLayout(
    std::vector<std::vector<Byte>> strings,
    GrammarDictionaryLayout layout) {
  if (layout == GrammarDictionaryLayout::kContainment) {
    return ConcatenateMaximal(std::move(strings));
  }
  return GreedySuperstring(std::move(strings));
}

}  // namespace grammar_layout_detail

class OverlappingGrammarLzOptimizer {
 public:
  explicit OverlappingGrammarLzOptimizer(
      Model const& model, GrammarLzOptions options = {})
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

  grammar_layout_detail::DictionaryLayout BuildPrefixLayoutForRuleLimit(
      std::size_t limit, GrammarDictionaryLayout layout) const {
    limit = std::min(limit, candidates_.size());
    auto selected = std::vector<std::uint8_t>(model_.rules.size(), 0);
    for (std::size_t i = 0; i < limit; ++i) {
      selected[candidates_[i].rule_index] = 1;
    }

    auto strings = std::vector<std::vector<Byte>>{};
    strings.reserve(limit);
    for (auto rule : topological_rules_) {
      if (selected[rule] != 0) {
        strings.push_back(rule_expansions_[rule]);
      }
    }
    return grammar_layout_detail::BuildLayout(std::move(strings), layout);
  }

  std::vector<Byte> BuildPrefixForRuleLimit(
      std::size_t limit, GrammarDictionaryLayout layout) const {
    return BuildPrefixLayoutForRuleLimit(limit, layout).bytes;
  }

  GrammarLzFrame PackForRuleLimit(
      std::size_t limit, GrammarDictionaryLayout layout) const {
    limit = std::min(limit, candidates_.size());
    auto selected_indices = std::vector<std::size_t>{};
    selected_indices.reserve(limit);
    for (std::size_t i = 0; i < limit; ++i) {
      selected_indices.push_back(candidates_[i].rule_index);
    }

    auto dictionary = BuildPrefixLayoutForRuleLimit(limit, layout);
    auto encoder = grammar_lz_detail::CommandEncoder{options_};
    encoder.EncodeSegment(dictionary.bytes);
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

  GrammarLzFrame PackBest(GrammarDictionaryLayout layout) const {
    auto best = GrammarLzFrame{};
    bool initialized = false;
    for (auto limit : TrialRuleLimits()) {
      auto frame = PackForRuleLimit(limit, layout);
      if (!initialized || frame.bytes.size() < best.bytes.size() ||
          (frame.bytes.size() == best.bytes.size() &&
           frame.stats.prefix_bytes < best.stats.prefix_bytes)) {
        best = std::move(frame);
        initialized = true;
      }
    }
    return best;
  }

  GrammarLzFrame PackBestAnyLayout() const {
    auto containment = PackBest(GrammarDictionaryLayout::kContainment);
    auto superstring = PackBest(
        GrammarDictionaryLayout::kGreedySuperstring);
    if (superstring.bytes.size() < containment.bytes.size() ||
        (superstring.bytes.size() == containment.bytes.size() &&
         superstring.stats.prefix_bytes < containment.stats.prefix_bytes)) {
      return superstring;
    }
    return containment;
  }

 private:
  void Analyze() {
    rule_expansions_.resize(model_.rules.size());
    states_.assign(model_.rules.size(), 0);

    for (auto symbol : model_.stream) {
      if (symbol >= kFirstRuleSymbol) {
        ExpandRule(RuleIndex(symbol));
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
      throw FormatError{"overlapping grammar-lz references unknown rule"};
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
      auto const& expansion = ExpandRule(RuleIndex(symbol));
      if (expansion.size() >
          std::numeric_limits<std::size_t>::max() - result.size()) {
        throw FormatError{"overlapping grammar-lz expansion overflow"};
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
      throw FormatError{"overlapping grammar-lz rule graph is cyclic"};
    }
    states_[rule] = 1;
    rule_expansions_[rule] = ExpandLayer(model_.rules[rule].symbols);
    states_[rule] = 2;
    topological_rules_.push_back(rule);
    return rule_expansions_[rule];
  }

  Model const& model_;
  GrammarLzOptions options_;
  std::vector<std::vector<Byte>> rule_expansions_;
  std::vector<std::uint8_t> states_;
  std::vector<std::size_t> topological_rules_;
  std::vector<grammar_lz_detail::Candidate> candidates_;
  std::vector<Byte> root_;
};

inline GrammarLzFrame PackOverlappingGrammarLz(
    Model const& model, GrammarLzOptions options = {}) {
  return OverlappingGrammarLzOptimizer{model, options}.PackBestAnyLayout();
}

}  // namespace ae::compression::experimental

#endif  // AE_COMPRESSION_EXPERIMENTAL_GRAMMAR_LZ_LAYOUT_HPP_
