#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "ae_compression/ae_compression.hpp"
#include "ae_compression/experimental/effective_compressor.hpp"
#include "ae_compression/experimental/grammar_lz_layout.hpp"

namespace {

using ae::compression::Byte;
using ae::compression::Model;
using ae::compression::Rule;
using ae::compression::Symbol;
using ae::compression::kFirstRuleSymbol;
using ae::compression::experimental::GrammarDictionaryLayout;
using ae::compression::experimental::GrammarLzOptions;
using ae::compression::experimental::OverlappingGrammarLzOptimizer;

std::vector<Byte> Bytes(std::string const& value) {
  return {value.begin(), value.end()};
}

std::vector<Symbol> Symbols(std::string const& value) {
  auto result = std::vector<Symbol>{};
  for (auto ch : value) {
    result.push_back(static_cast<unsigned char>(ch));
  }
  return result;
}

Model ManualModel() {
  auto model = Model{};
  model.rules.push_back(Rule{Symbols("abc")});
  model.rules.push_back(
      Rule{{kFirstRuleSymbol, static_cast<Symbol>('d')}});
  model.stream = {kFirstRuleSymbol, kFirstRuleSymbol + 1,
                  kFirstRuleSymbol + 1};
  return model;
}

void TestNestedRangesShareStorage() {
  auto model = ManualModel();
  auto const expected = Bytes("abcabcdabcd");
  auto optimizer = OverlappingGrammarLzOptimizer{model};
  assert(optimizer.CandidateCount() == 2);

  for (auto layout : {GrammarDictionaryLayout::kContainment,
                      GrammarDictionaryLayout::kGreedySuperstring}) {
    auto dictionary = optimizer.BuildPrefixLayoutForRuleLimit(2, layout);
    assert(dictionary.bytes == Bytes("abcd"));
    assert(dictionary.fragments == 1);

    auto frame = optimizer.PackForRuleLimit(2, layout);
    assert(frame.stats.prefix_bytes == 4);
    assert(frame.stats.output_bytes == expected.size());
    assert(ae::compression::experimental::DecodeGrammarLz(frame.bytes) ==
           expected);
  }

  auto old_optimizer =
      ae::compression::experimental::GrammarLzOptimizer{model};
  auto old = old_optimizer.PackForRuleLimit(2);
  auto overlapping = optimizer.PackForRuleLimit(
      2, GrammarDictionaryLayout::kContainment);
  assert(old.stats.prefix_bytes == 7);
  assert(overlapping.stats.prefix_bytes == 4);
  assert(overlapping.bytes.size() < old.bytes.size());
}

void TestGeneratedModels() {
  auto samples = std::vector<std::vector<Byte>>{};
  samples.push_back({});
  samples.push_back(Bytes("abcabcdabcd"));
  samples.push_back(Bytes("abcabcabcabcabcabc"));
  samples.push_back(Bytes("hello hello hello hello"));

  auto repeating = std::string{};
  for (int i = 0; i < 128; ++i) {
    repeating += "temperature=21.25;humidity=40;device=alpha;";
  }
  samples.push_back(Bytes(repeating));

  auto options = GrammarLzOptions{};
  options.max_candidates = 24;
  options.max_chain = 64;

  for (auto const& input : samples) {
    auto baseline = ae::compression::Compress(input);
    auto effective_options =
        ae::compression::experimental::EffectiveCompressionOptions{};
    effective_options.effective_finalize = false;
    auto effective = ae::compression::experimental::CompressEffective(
        input, effective_options);

    for (auto const* model : {&baseline, &effective}) {
      auto optimizer = OverlappingGrammarLzOptimizer{*model, options};
      for (auto layout : {GrammarDictionaryLayout::kContainment,
                          GrammarDictionaryLayout::kGreedySuperstring}) {
        auto all = optimizer.PackForRuleLimit(
            optimizer.CandidateCount(), layout);
        auto best = optimizer.PackBest(layout);
        assert(ae::compression::experimental::DecodeGrammarLz(all.bytes) ==
               input);
        assert(ae::compression::experimental::DecodeGrammarLz(best.bytes) ==
               input);
        assert(best.bytes.size() <= all.bytes.size());
      }
    }
  }
}

}  // namespace

int main() {
  TestNestedRangesShareStorage();
  TestGeneratedModels();
}
