/*
 * Copyright 2026 Aethernet Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#ifndef AE_COMPRESSION_COMPRESSOR_HPP_
#define AE_COMPRESSION_COMPRESSOR_HPP_

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "ae_compression/format.hpp"
#include "ae_compression/model.hpp"

namespace ae::compression {

namespace detail {

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

inline double EntropyBits(std::vector<int> const& values) {
  if (values.empty()) {
    return 0.0;
  }

  auto counts = std::map<int, std::size_t>{};
  for (auto value : values) {
    ++counts[value];
  }

  double entropy = 0.0;
  auto const total = static_cast<double>(values.size());
  for (auto const& [value, count] : counts) {
    (void)value;
    auto const p = static_cast<double>(count) / total;
    entropy -= p * std::log2(p);
  }
  return entropy * total;
}

}  // namespace detail

class Compressor {
 public:
  explicit Compressor(CompressionOptions options = {}) : options_{options} {}

  Model Compress(std::span<Byte const> data) const {
    auto model = Model{};
    model.stream.reserve(data.size());
    for (auto byte : data) {
      model.stream.push_back(byte);
    }
    Compress(model);
    return model;
  }

  std::vector<Byte> Encode(std::span<Byte const> data,
                           PackOptions pack_options = {}) const {
    auto model = Compress(data);
    auto dictionary_frame = PackArithmeticDictionary(model);
    if (pack_options.raw_fallback) {
      auto raw_frame = PackRaw(data);
      if (raw_frame.size() <= dictionary_frame.size()) {
        return raw_frame;
      }
    }
    return dictionary_frame;
  }

 private:
  static int CountGlobalOccurrences(Model const& model,
                                    std::vector<Symbol> const& pattern) {
    if (pattern.empty()) {
      return 0;
    }

    int count = 0;
    auto count_layer = [&](std::vector<Symbol> const& layer) {
      if (layer.size() < pattern.size()) {
        return;
      }

      auto it = layer.begin();
      while (true) {
        it = std::search(it, layer.end(), pattern.begin(), pattern.end());
        if (it == layer.end()) {
          break;
        }
        ++count;
        it += static_cast<std::ptrdiff_t>(pattern.size());
      }
    };

    count_layer(model.stream);
    for (auto const& rule : model.rules) {
      count_layer(rule.symbols);
    }
    return count;
  }

  static void ReplaceGlobally(Model& model, std::vector<Symbol> const& pattern,
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
    if (model.rules.empty()) {
      return;
    }
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
        if (symbol >= kFirstRuleSymbol) {
          auto found = id_map.find(symbol);
          if (found != id_map.end()) {
            symbol = found->second;
          }
        }
      }
    };

    reindex_layer(model.stream);
    for (auto& rule : new_rules) {
      reindex_layer(rule.symbols);
    }
    model.rules = std::move(new_rules);
  }

  static void Finalize(Model& model) {
    bool changed = true;
    while (changed) {
      changed = false;
      auto usage = std::map<Symbol, int>{};
      auto count_layer = [&](std::vector<Symbol> const& layer) {
        for (auto symbol : layer) {
          if (symbol >= kFirstRuleSymbol) {
            ++usage[symbol];
          }
        }
      };

      count_layer(model.stream);
      for (auto const& rule : model.rules) {
        count_layer(rule.symbols);
      }

      auto rules_to_inline = std::vector<Symbol>{};
      for (std::size_t i = 0; i < model.rules.size(); ++i) {
        if (model.rules[i].symbols.empty()) {
          continue;
        }
        auto const symbol = kFirstRuleSymbol + static_cast<Symbol>(i);
        auto const count = usage[symbol];
        auto const size = static_cast<int>(model.rules[i].symbols.size());
        if (count <= 1 || (count == 2 && size == 2)) {
          rules_to_inline.push_back(symbol);
        }
      }

      if (rules_to_inline.empty()) {
        break;
      }
      changed = true;

      auto inline_content = std::map<Symbol, std::vector<Symbol>>{};
      for (auto symbol : rules_to_inline) {
        auto const rule_index = static_cast<std::size_t>(symbol - kFirstRuleSymbol);
        inline_content[symbol] = std::move(model.rules[rule_index].symbols);
        model.rules[rule_index].symbols.clear();
      }

      bool inner_changed = true;
      while (inner_changed) {
        inner_changed = false;
        for (auto& [symbol, symbols] : inline_content) {
          (void)symbol;
          auto flattened = std::vector<Symbol>{};
          auto modified = false;
          for (auto inner_symbol : symbols) {
            auto found = inline_content.find(inner_symbol);
            if (found != inline_content.end()) {
              flattened.insert(flattened.end(), found->second.begin(),
                               found->second.end());
              modified = true;
              inner_changed = true;
            } else {
              flattened.push_back(inner_symbol);
            }
          }
          if (modified) {
            symbols = std::move(flattened);
          }
        }
      }

      auto apply_inline = [&](std::vector<Symbol>& layer) {
        auto next = std::vector<Symbol>{};
        next.reserve(layer.size());
        auto modified = false;
        for (auto symbol : layer) {
          auto found = inline_content.find(symbol);
          if (found != inline_content.end()) {
            next.insert(next.end(), found->second.begin(), found->second.end());
            modified = true;
          } else {
            next.push_back(symbol);
          }
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
      std::size_t total_symbols = model.stream.size();
      for (auto const& rule : model.rules) {
        max_layer_length = std::max(max_layer_length, rule.symbols.size());
        total_symbols += rule.symbols.size();
      }
      (void)total_symbols;

      auto const start_len =
          std::min(max_layer_length / 2, options_.max_seed_length);
      if (start_len < 2) {
        break;
      }

      auto best_pattern = std::vector<Symbol>{};
      int best_count = 0;
      bool found_seed = false;

      for (std::size_t len = start_len; len >= 2; --len) {
        auto candidates = std::map<detail::SpanKey, int>{};

        auto collect_layer = [&](std::vector<Symbol> const& layer) {
          if (layer.size() < len) {
            return;
          }
          auto const* ptr = layer.data();
          for (std::size_t i = 0; i <= layer.size() - len; ++i) {
            ++candidates[detail::SpanKey{ptr + i, len}];
          }
        };

        collect_layer(model.stream);
        for (auto const& rule : model.rules) {
          collect_layer(rule.symbols);
        }

        for (auto const& [span, raw_count] : candidates) {
          if (raw_count < 2) {
            continue;
          }

          auto pattern = std::vector<Symbol>(span.ptr, span.ptr + span.len);
          auto const real_count = CountGlobalOccurrences(model, pattern);
          if (pattern.size() == 2 && real_count < 3) {
            continue;
          }

          auto const score =
              (static_cast<int>(pattern.size()) - 1) * (real_count - 1);
          if (score > options_.min_score) {
            best_pattern = std::move(pattern);
            best_count = real_count;
            found_seed = true;
            break;
          }
        }
        if (found_seed || len == 2) {
          break;
        }
      }

      if (!found_seed) {
        break;
      }

      if (options_.extend_seed) {
        while (true) {
          Symbol next_symbol = 0;
          bool can_extend = false;

          auto find_extension = [&](std::vector<Symbol> const& layer) {
            if (can_extend || layer.size() <= best_pattern.size()) {
              return;
            }
            auto it =
                std::search(layer.begin(), layer.end(), best_pattern.begin(),
                            best_pattern.end());
            if (it != layer.end() &&
                (it + static_cast<std::ptrdiff_t>(best_pattern.size())) !=
                    layer.end()) {
              next_symbol =
                  *(it + static_cast<std::ptrdiff_t>(best_pattern.size()));
              can_extend = true;
            }
          };

          find_extension(model.stream);
          for (auto const& rule : model.rules) {
            find_extension(rule.symbols);
          }

          if (!can_extend) {
            break;
          }

          auto extended = best_pattern;
          extended.push_back(next_symbol);
          auto const extended_count = CountGlobalOccurrences(model, extended);
          if (extended_count >= 2) {
            best_pattern = std::move(extended);
            best_count = extended_count;
          } else {
            break;
          }
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

  CompressionOptions options_;
};

inline Model Compress(std::span<Byte const> data,
                      CompressionOptions options = {}) {
  return Compressor{options}.Compress(data);
}

inline std::vector<Byte> Encode(std::span<Byte const> data,
                                CompressionOptions options = {},
                                PackOptions pack_options = {}) {
  return Compressor{options}.Encode(data, pack_options);
}

inline CompressionStats Analyze(Model const& model, std::size_t original_size) {
  auto stats = CompressionStats{};
  stats.original_size = original_size;
  stats.top_level_symbols = model.stream.size();
  stats.rule_count = model.rules.size();

  auto symbol_counts = std::map<Symbol, std::size_t>{};
  auto rule_counts = std::map<Symbol, int>{};
  auto count_layer = [&](std::vector<Symbol> const& layer) {
    for (auto symbol : layer) {
      ++symbol_counts[symbol];
      ++stats.total_symbols;
      if (symbol >= kFirstRuleSymbol) {
        ++rule_counts[symbol];
      }
    }
  };

  count_layer(model.stream);
  for (auto const& rule : model.rules) {
    count_layer(rule.symbols);
  }
  stats.unique_symbols = symbol_counts.size();

  double payload_entropy_bits = 0.0;
  if (stats.total_symbols != 0) {
    for (auto const& [symbol, count] : symbol_counts) {
      (void)symbol;
      auto const p = static_cast<double>(count) /
                     static_cast<double>(stats.total_symbols);
      payload_entropy_bits -= p * std::log2(p);
    }
    payload_entropy_bits *= static_cast<double>(stats.total_symbols);
  }

  auto twin_lengths = std::vector<int>{};
  auto common_lengths = std::vector<int>{};
  auto common_counts = std::vector<int>{};
  for (std::size_t i = 0; i < model.rules.size(); ++i) {
    auto const symbol = kFirstRuleSymbol + static_cast<Symbol>(i);
    auto const count = rule_counts[symbol];
    auto const len = static_cast<int>(model.rules[i].symbols.size());
    if (count == 2) {
      twin_lengths.push_back(len);
    } else {
      common_lengths.push_back(len);
      common_counts.push_back(count);
    }
  }

  std::sort(twin_lengths.begin(), twin_lengths.end());
  auto twin_deltas = std::vector<int>{};
  int previous = 0;
  for (auto len : twin_lengths) {
    twin_deltas.push_back(len - previous);
    previous = len;
  }

  auto common_pairs = std::vector<std::pair<int, int>>{};
  for (std::size_t i = 0; i < common_lengths.size(); ++i) {
    common_pairs.push_back({common_lengths[i], common_counts[i]});
  }
  std::sort(common_pairs.begin(), common_pairs.end());

  auto common_length_deltas = std::vector<int>{};
  auto common_count_values = std::vector<int>{};
  previous = 0;
  for (auto [len, count] : common_pairs) {
    common_length_deltas.push_back(len - previous);
    previous = len;
    common_count_values.push_back(count);
  }

  auto const header_bits =
      detail::EntropyBits(twin_deltas) +
      detail::EntropyBits(common_length_deltas) +
      detail::EntropyBits(common_count_values);

  stats.estimated_payload_bytes =
      static_cast<std::size_t>(payload_entropy_bits / 8.0);
  stats.estimated_header_bytes = static_cast<std::size_t>(header_bits / 8.0) + 4;
  stats.estimated_total_bytes =
      stats.estimated_payload_bytes + stats.estimated_header_bytes;
  return stats;
}

}  // namespace ae::compression

#endif  // AE_COMPRESSION_COMPRESSOR_HPP_
