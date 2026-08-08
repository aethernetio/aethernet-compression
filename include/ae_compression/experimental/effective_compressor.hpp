/*
 * Copyright 2026 Aethernet Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#ifndef AE_COMPRESSION_EXPERIMENTAL_EFFECTIVE_COMPRESSOR_HPP_
#define AE_COMPRESSION_EXPERIMENTAL_EFFECTIVE_COMPRESSOR_HPP_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <vector>

#include "ae_compression/model.hpp"

namespace ae::compression::experimental {

using Weight = std::uint64_t;

struct EffectiveCompressionOptions {
  std::size_t max_seed_length = 16;
  Weight min_score = 4;
  bool extend_seed = true;
  // If true, pruning also uses expanded multiplicity. This is included only to
  // test the hypothesis; structural pruning is normally the correct choice for
  // a serialized grammar.
  bool effective_finalize = false;
};

namespace detail {

inline Weight SaturatingAdd(Weight left, Weight right) {
  auto const max = std::numeric_limits<Weight>::max();
  return left > max - right ? max : left + right;
}

inline Weight SaturatingMultiply(Weight left, Weight right) {
  if (left == 0 || right == 0) {
    return 0;
  }
  auto const max = std::numeric_limits<Weight>::max();
  return left > max / right ? max : left * right;
}

struct SpanKey {
  Symbol const* ptr = nullptr;
  std::size_t len = 0;

  bool operator<(SpanKey const& other) const {
    if (len != other.len) {
      return len < other.len;
    }
    return std::lexicographical_compare(ptr, ptr + len, other.ptr,
                                        other.ptr + other.len);
  }
};

inline std::size_t CountLayerOccurrences(
    std::vector<Symbol> const& layer,
    std::span<Symbol const> pattern) {
  if (pattern.empty() || layer.size() < pattern.size()) {
    return 0;
  }

  std::size_t count = 0;
  auto it = layer.begin();
  while (true) {
    it = std::search(it, layer.end(), pattern.begin(), pattern.end());
    if (it == layer.end()) {
      return count;
    }
    ++count;
    it += static_cast<std::ptrdiff_t>(pattern.size());
  }
}

}  // namespace detail

// Number of times each rule is expanded from the top-level stream. For a DAG:
//   multiplicity[child] = direct_stream_uses(child)
//       + sum(multiplicity[parent] * references(parent, child)).
inline std::vector<Weight> RuleMultiplicities(Model const& model) {
  auto const rule_count = model.rules.size();
  auto multiplicities = std::vector<Weight>(rule_count, 0);
  auto indegrees = std::vector<std::size_t>(rule_count, 0);

  for (auto symbol : model.stream) {
    if (symbol < kFirstRuleSymbol) {
      continue;
    }
    auto const index = static_cast<std::size_t>(symbol - kFirstRuleSymbol);
    if (index < rule_count) {
      multiplicities[index] =
          detail::SaturatingAdd(multiplicities[index], 1);
    }
  }

  for (auto const& rule : model.rules) {
    for (auto symbol : rule.symbols) {
      if (symbol < kFirstRuleSymbol) {
        continue;
      }
      auto const index = static_cast<std::size_t>(symbol - kFirstRuleSymbol);
      if (index < rule_count) {
        ++indegrees[index];
      }
    }
  }

  auto queue = std::vector<std::size_t>{};
  queue.reserve(rule_count);
  for (std::size_t i = 0; i < rule_count; ++i) {
    if (indegrees[i] == 0) {
      queue.push_back(i);
    }
  }

  std::size_t queue_pos = 0;
  while (queue_pos < queue.size()) {
    auto const parent = queue[queue_pos++];
    for (auto symbol : model.rules[parent].symbols) {
      if (symbol < kFirstRuleSymbol) {
        continue;
      }
      auto const child =
          static_cast<std::size_t>(symbol - kFirstRuleSymbol);
      if (child >= rule_count) {
        continue;
      }
      multiplicities[child] = detail::SaturatingAdd(
          multiplicities[child], multiplicities[parent]);
      if (--indegrees[child] == 0) {
        queue.push_back(child);
      }
    }
  }

  // A cycle is invalid for the decoder. Leave nodes in a cycle at their direct
  // top-level multiplicity; the normal round-trip validation will reject it.
  return multiplicities;
}

inline std::size_t CountStructuralOccurrences(
    Model const& model, std::span<Symbol const> pattern) {
  auto count = detail::CountLayerOccurrences(model.stream, pattern);
  for (auto const& rule : model.rules) {
    count += detail::CountLayerOccurrences(rule.symbols, pattern);
  }
  return count;
}

inline Weight CountEffectiveOccurrences(
    Model const& model, std::span<Symbol const> pattern,
    std::span<Weight const> multiplicities) {
  auto count = static_cast<Weight>(
      detail::CountLayerOccurrences(model.stream, pattern));
  for (std::size_t i = 0; i < model.rules.size(); ++i) {
    auto const structural =
        static_cast<Weight>(detail::CountLayerOccurrences(
            model.rules[i].symbols, pattern));
    auto const weight = i < multiplicities.size() ? multiplicities[i] : 0;
    count = detail::SaturatingAdd(
        count, detail::SaturatingMultiply(structural, weight));
  }
  return count;
}

class EffectiveCompressor {
 public:
  explicit EffectiveCompressor(EffectiveCompressionOptions options = {})
      : options_{options} {}

  Model Compress(std::span<Byte const> data) const {
    auto model = Model{};
    model.stream.reserve(data.size());
    for (auto byte : data) {
      model.stream.push_back(byte);
    }
    Compress(model);
    return model;
  }

 private:
  static void ReplaceGlobally(Model& model,
                              std::vector<Symbol> const& pattern,
                              Symbol rule_symbol) {
    auto replace_layer = [&](std::vector<Symbol>& layer) {
      if (layer.size() < pattern.size()) {
        return;
      }

      auto next_layer = std::vector<Symbol>{};
      next_layer.reserve(layer.size());
      auto it = layer.begin();
      while (it != layer.end()) {
        auto match = std::search(it, layer.end(), pattern.begin(), pattern.end());
        next_layer.insert(next_layer.end(), it, match);
        if (match == layer.end()) {
          break;
        }
        next_layer.push_back(rule_symbol);
        it = match + static_cast<std::ptrdiff_t>(pattern.size());
      }
      layer = std::move(next_layer);
    };

    replace_layer(model.stream);
    for (std::size_t i = 0; i + 1 < model.rules.size(); ++i) {
      replace_layer(model.rules[i].symbols);
    }
  }

  static void ReindexAfterPruning(Model& model) {
    auto id_map = std::map<Symbol, Symbol>{};
    auto new_rules = std::vector<Rule>{};
    new_rules.reserve(model.rules.size());

    Symbol next_symbol = kFirstRuleSymbol;
    for (std::size_t i = 0; i < model.rules.size(); ++i) {
      if (!model.rules[i].symbols.empty()) {
        id_map[kFirstRuleSymbol + static_cast<Symbol>(i)] = next_symbol++;
        new_rules.push_back(std::move(model.rules[i]));
      }
    }

    auto reindex_layer = [&](std::vector<Symbol>& layer) {
      for (auto& symbol : layer) {
        if (symbol < kFirstRuleSymbol) {
          continue;
        }
        auto found = id_map.find(symbol);
        if (found != id_map.end()) {
          symbol = found->second;
        }
      }
    };

    reindex_layer(model.stream);
    for (auto& rule : new_rules) {
      reindex_layer(rule.symbols);
    }
    model.rules = std::move(new_rules);
  }

  void Finalize(Model& model) const {
    bool changed = true;
    while (changed) {
      changed = false;
      auto usage = std::vector<Weight>(model.rules.size(), 0);

      if (options_.effective_finalize) {
        usage = RuleMultiplicities(model);
      } else {
        auto count_layer = [&](std::vector<Symbol> const& layer) {
          for (auto symbol : layer) {
            if (symbol < kFirstRuleSymbol) {
              continue;
            }
            auto const index =
                static_cast<std::size_t>(symbol - kFirstRuleSymbol);
            if (index < usage.size()) {
              usage[index] = detail::SaturatingAdd(usage[index], 1);
            }
          }
        };
        count_layer(model.stream);
        for (auto const& rule : model.rules) {
          count_layer(rule.symbols);
        }
      }

      auto rules_to_inline = std::vector<Symbol>{};
      for (std::size_t i = 0; i < model.rules.size(); ++i) {
        if (model.rules[i].symbols.empty()) {
          continue;
        }
        auto const count = usage[i];
        auto const size = model.rules[i].symbols.size();
        if (count <= 1 || (count == 2 && size == 2)) {
          rules_to_inline.push_back(kFirstRuleSymbol +
                                    static_cast<Symbol>(i));
        }
      }

      if (rules_to_inline.empty()) {
        break;
      }
      changed = true;

      auto inline_content = std::map<Symbol, std::vector<Symbol>>{};
      for (auto symbol : rules_to_inline) {
        auto const index =
            static_cast<std::size_t>(symbol - kFirstRuleSymbol);
        inline_content[symbol] = std::move(model.rules[index].symbols);
        model.rules[index].symbols.clear();
      }

      bool inner_changed = true;
      while (inner_changed) {
        inner_changed = false;
        for (auto& [symbol, symbols] : inline_content) {
          (void)symbol;
          auto flattened = std::vector<Symbol>{};
          bool modified = false;
          for (auto inner_symbol : symbols) {
            auto found = inline_content.find(inner_symbol);
            if (found == inline_content.end()) {
              flattened.push_back(inner_symbol);
              continue;
            }
            flattened.insert(flattened.end(), found->second.begin(),
                             found->second.end());
            modified = true;
            inner_changed = true;
          }
          if (modified) {
            symbols = std::move(flattened);
          }
        }
      }

      auto apply_inline = [&](std::vector<Symbol>& layer) {
        auto next = std::vector<Symbol>{};
        next.reserve(layer.size());
        bool modified = false;
        for (auto symbol : layer) {
          auto found = inline_content.find(symbol);
          if (found == inline_content.end()) {
            next.push_back(symbol);
            continue;
          }
          next.insert(next.end(), found->second.begin(), found->second.end());
          modified = true;
        }
        if (modified) {
          layer = std::move(next);
        }
      };

      apply_inline(model.stream);
      for (auto& rule : model.rules) {
        if (!rule.symbols.empty()) {
          apply_inline(rule.symbols);
        }
      }
    }

    ReindexAfterPruning(model);
  }

  void Compress(Model& model) const {
    while (true) {
      std::size_t max_layer_length = model.stream.size();
      for (auto const& rule : model.rules) {
        max_layer_length = std::max(max_layer_length, rule.symbols.size());
      }

      auto const start_len =
          std::min(max_layer_length / 2, options_.max_seed_length);
      if (start_len < 2) {
        break;
      }

      auto const multiplicities = RuleMultiplicities(model);
      auto best_pattern = std::vector<Symbol>{};
      Weight best_count = 0;
      Weight best_score = 0;

      for (std::size_t len = start_len; len >= 2; --len) {
        auto candidates = std::map<detail::SpanKey, Weight>{};

        auto collect_layer = [&](std::vector<Symbol> const& layer,
                                 Weight weight) {
          if (weight == 0 || layer.size() < len) {
            return;
          }
          auto const* ptr = layer.data();
          for (std::size_t i = 0; i <= layer.size() - len; ++i) {
            auto& count = candidates[detail::SpanKey{ptr + i, len}];
            count = detail::SaturatingAdd(count, weight);
          }
        };

        collect_layer(model.stream, 1);
        for (std::size_t i = 0; i < model.rules.size(); ++i) {
          collect_layer(model.rules[i].symbols, multiplicities[i]);
        }

        bool found_at_length = false;
        for (auto const& [span, raw_count] : candidates) {
          if (raw_count < 2) {
            continue;
          }
          auto pattern = std::vector<Symbol>(span.ptr, span.ptr + span.len);

          // Expanded multiplicity alone must not create an alias for a pattern
          // that occurs only once in the serialized grammar.
          auto const structural_count =
              CountStructuralOccurrences(model, pattern);
          if (structural_count < 2 ||
              (pattern.size() == 2 && structural_count < 3)) {
            continue;
          }

          auto const effective_count = CountEffectiveOccurrences(
              model, pattern, multiplicities);
          auto const score = detail::SaturatingMultiply(
              static_cast<Weight>(pattern.size() - 1),
              effective_count - 1);
          if (score <= options_.min_score || score <= best_score) {
            continue;
          }

          best_pattern = std::move(pattern);
          best_count = effective_count;
          best_score = score;
          found_at_length = true;
        }

        if (found_at_length || len == 2) {
          break;
        }
      }

      if (best_pattern.empty()) {
        break;
      }

      if (options_.extend_seed) {
        while (true) {
          auto next_counts = std::map<Symbol, Weight>{};

          auto collect_extensions = [&](std::vector<Symbol> const& layer,
                                        Weight weight) {
            if (weight == 0 || layer.size() <= best_pattern.size()) {
              return;
            }
            auto it = layer.begin();
            while (true) {
              it = std::search(it, layer.end(), best_pattern.begin(),
                               best_pattern.end());
              if (it == layer.end()) {
                return;
              }
              auto const next =
                  it + static_cast<std::ptrdiff_t>(best_pattern.size());
              if (next != layer.end()) {
                auto& count = next_counts[*next];
                count = detail::SaturatingAdd(count, weight);
              }
              it += static_cast<std::ptrdiff_t>(best_pattern.size());
            }
          };

          collect_extensions(model.stream, 1);
          for (std::size_t i = 0; i < model.rules.size(); ++i) {
            collect_extensions(model.rules[i].symbols, multiplicities[i]);
          }

          auto best_extended = std::vector<Symbol>{};
          Weight extended_count = 0;
          for (auto const& [next_symbol, raw_count] : next_counts) {
            if (raw_count < 2) {
              continue;
            }
            auto candidate = best_pattern;
            candidate.push_back(next_symbol);
            if (CountStructuralOccurrences(model, candidate) < 2) {
              continue;
            }
            auto const count = CountEffectiveOccurrences(
                model, candidate, multiplicities);
            if (count > extended_count) {
              best_extended = std::move(candidate);
              extended_count = count;
            }
          }

          if (best_extended.empty()) {
            break;
          }
          best_pattern = std::move(best_extended);
          best_count = extended_count;
        }
      }

      (void)best_count;
      auto const new_symbol =
          kFirstRuleSymbol + static_cast<Symbol>(model.rules.size());
      model.rules.push_back(Rule{best_pattern});
      ReplaceGlobally(model, best_pattern, new_symbol);
    }

    Finalize(model);
  }

  EffectiveCompressionOptions options_;
};

inline Model CompressEffective(
    std::span<Byte const> data,
    EffectiveCompressionOptions options = {}) {
  return EffectiveCompressor{options}.Compress(data);
}

}  // namespace ae::compression::experimental

#endif  // AE_COMPRESSION_EXPERIMENTAL_EFFECTIVE_COMPRESSOR_HPP_
